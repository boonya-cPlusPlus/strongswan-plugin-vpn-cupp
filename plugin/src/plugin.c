/* 修改内容：创建插件入口实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#include <strongswan.h>
#include <daemon.h>						/* charon, lib */
#include <library.h>
#include <plugins/plugin.h>
#include <plugins/plugin_feature.h>
#include <attributes/attribute_manager.h>
#include <bus/bus.h>
#include <processing/processor.h>
#include <processing/jobs/callback_job.h>
#include <processing/jobs/job.h>
#include <utils/utils.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "policy_engine.h"
#include "ip_allocator.h"
#include "event_listener.h"
#include "cupp_log.h"

#define CUPP_PLUGIN_NAME	"user-policy"
#define CUPP_DEFAULT_POLICY	"/etc/strongswan/cupp/policy.yaml"
#define CUPP_DEFAULT_POOL	"cupp"
#define CUPP_DEFAULT_LEASE	3600
#define CUPP_DEFAULT_SWEEP	60

typedef struct private_user_policy_plugin_t private_user_policy_plugin_t;

/*
 * 引用计数说明：
 *   plugin 实例本身持 1 ref（constructor 取得，destroy 释放）；
 *   每个在途的 sweep callback_job 持 1 ref（job 启动时 ref++，cleanup 时 ref--）。
 *   最后一个 ref 释放时回收全部资源。
 *   这样保证 job 的 cb/cancel/cleanup 即便在 plugin unload 后被处理器调用，
 *   访问的 this 仍有效（直到 cleanup 释放最后一个 ref）。
 */
struct private_user_policy_plugin_t {
	plugin_t public;
	int refcnt;					/* 受 ref_lock 保护 */
	pthread_mutex_t ref_lock;
	pthread_mutex_t sweep_lock;/* 序列化 sweep 执行与 unload */
	volatile bool terminating;	/* unload 后置位，sweep 据此停止重排 */
	policy_engine_t *policy;
	ip_allocator_t *allocator;
	event_listener_t *listener;
	char *policy_path;
	char *pool_name;
	uint32_t lease_timeout;
	uint32_t sweep_interval;	/* 秒 */
};

/* ---- refcount ---- */

static void plugin_ref(private_user_policy_plugin_t *this)
{
	pthread_mutex_lock(&this->ref_lock);
	this->refcnt++;
	pthread_mutex_unlock(&this->ref_lock);
}

static void plugin_destroy_internal(private_user_policy_plugin_t *this)
{
	if (this->listener)
	{
		event_listener_destroy(this->listener);
	}
	if (this->allocator)
	{
		ip_allocator_destroy(this->allocator);
	}
	if (this->policy)
	{
		policy_engine_destroy(this->policy);
	}
	pthread_mutex_destroy(&this->ref_lock);
	pthread_mutex_destroy(&this->sweep_lock);
	free(this->policy_path);
	free(this->pool_name);
	free(this);
}

static void plugin_unref(private_user_policy_plugin_t *this)
{
	bool last = FALSE;

	pthread_mutex_lock(&this->ref_lock);
	if (--this->refcnt <= 0)
	{
		last = TRUE;
	}
	pthread_mutex_unlock(&this->ref_lock);
	if (last)
	{
		plugin_destroy_internal(this);
	}
}

/* ---- sweep job ---- */

static job_requeue_t sweep_cb(private_user_policy_plugin_t *this)
{
	if (this->terminating)
	{
		return JOB_REQUEUE_NONE;
	}
	pthread_mutex_lock(&this->sweep_lock);
	if (!this->terminating && this->allocator)
	{
		this->allocator->sweep(this->allocator);
	}
	pthread_mutex_unlock(&this->sweep_lock);
	if (this->terminating)
	{
		return JOB_REQUEUE_NONE;
	}
	return JOB_RESCHEDULE_MS((u_int)this->sweep_interval * 1000);
}

static bool sweep_cancel(private_user_policy_plugin_t *this)
{
	return this->terminating;
}

static void sweep_cleanup(private_user_policy_plugin_t *this)
{
	/* job 生命周期结束（cancel 或 REQUEUE_NONE）时释放 job 持有的 ref */
	plugin_unref(this);
}

/* ---- plugin_t 实现 ---- */

METHOD(plugin_t, get_name, char*,
	private_user_policy_plugin_t *this)
{
	return CUPP_PLUGIN_NAME;
}

/*
 * 特征注册回调：reg=TRUE 时注册 provider/listener 并启动 sweep job；
 * reg=FALSE 时逆序注销并标记 terminating（sweep job 自行停止并 cleanup）。
 */
static bool plugin_cb(private_user_policy_plugin_t *this,
					  plugin_feature_t *feature, bool reg, void *data)
{
	callback_job_t *job;

	(void)feature; (void)data;

	if (reg)
	{
		charon->attributes->add_provider(charon->attributes,
										 &this->allocator->provider);
		charon->bus->add_listener(charon->bus, &this->listener->listener);

		/* 启动周期 sweep job：持 1 ref，cleanup 时释放 */
		plugin_ref(this);
		job = callback_job_create((callback_job_cb_t)sweep_cb, this,
								  (callback_job_cleanup_t)sweep_cleanup,
								  (callback_job_cancel_t)sweep_cancel);
		if (job)
		{
			lib->processor->queue_job(lib->processor, &job->job);
		}
		else
		{
			cupp_err("failed to create sweep job, orphan lease reclamation disabled");
			plugin_unref(this);
		}
		cupp_log("plugin registered (pool=%s lease_timeout=%us sweep=%us)",
				 this->pool_name, this->lease_timeout, this->sweep_interval);
	}
	else
	{
		charon->attributes->remove_provider(charon->attributes,
											&this->allocator->provider);
		charon->bus->remove_listener(charon->bus, &this->listener->listener);
		/* 标记 terminating：sweep job 检测到后停止重排并 cleanup（释放 job ref） */
		pthread_mutex_lock(&this->sweep_lock);
		this->terminating = TRUE;
		pthread_mutex_unlock(&this->sweep_lock);
		cupp_log("plugin unregistered");
	}
	return TRUE;
}

METHOD(plugin_t, get_features, int,
	private_user_policy_plugin_t *this, plugin_feature_t *features[])
{
	static plugin_feature_t f[] = {
		/* CUSTOM 特征 + 回调：注册时执行 plugin_cb(true)，卸载时 plugin_cb(false) */
		PLUGIN_CALLBACK((plugin_feature_callback_t)plugin_cb, NULL),
			PLUGIN_PROVIDE(CUSTOM, CUPP_PLUGIN_NAME),
	};
	*features = f;
	return countof(f);
}

METHOD(plugin_t, reload, bool,
	private_user_policy_plugin_t *this)
{
	if (!this->policy)
	{
		return FALSE;
	}
	return this->policy->reload(this->policy);
}

METHOD(plugin_t, destroy, void,
	private_user_policy_plugin_t *this)
{
	/* 释放 plugin 自身持有的 ref；sweep job 若仍在途，由其 cleanup 释放最后 ref */
	plugin_unref(this);
}

/* ---- 读取 strongswan.conf 配置 ---- */

static char *settings_get_str(const char *key, const char *def)
{
	return strdup(lib->settings->get_str(lib->settings, key, def));
}

static uint32_t settings_get_int(const char *key, uint32_t def)
{
	return (uint32_t)lib->settings->get_int(lib->settings, key, def);
}

/*
 * 插件构造函数。strongSwan plugin_loader 按文件名 libstrongswan-user-policy.so
 * 查找符号 user_policy_plugin_create（连字符转下划线）。
 *
 * 加载策略失败（policy.yaml 缺失/格式错误）时返回 NULL → charon 拒绝加载本插件
 * 并记录日志（fail-fast），避免以空策略运行导致所有用户无 IP。
 */
plugin_t *user_policy_plugin_create(void)
{
	private_user_policy_plugin_t *this;
	char key[128];

	INIT(this,
		.public = {
			.get_name = _get_name,
			.get_features = _get_features,
			.reload = _reload,
			.destroy = _destroy,
		},
		.refcnt = 1,	/* plugin 自身持有 */
	);
	pthread_mutex_init(&this->ref_lock, NULL);
	pthread_mutex_init(&this->sweep_lock, NULL);

	/* 配置项路径：charon.plugins.user-policy.<key>（lib->ns 通常为 "charon"） */
	snprintf(key, sizeof(key), "%s.plugins." CUPP_PLUGIN_NAME ".policy", lib->ns);
	this->policy_path = settings_get_str(key, CUPP_DEFAULT_POLICY);

	snprintf(key, sizeof(key), "%s.plugins." CUPP_PLUGIN_NAME ".pool_name", lib->ns);
	this->pool_name = settings_get_str(key, CUPP_DEFAULT_POOL);

	snprintf(key, sizeof(key), "%s.plugins." CUPP_PLUGIN_NAME ".lease_timeout", lib->ns);
	this->lease_timeout = settings_get_int(key, CUPP_DEFAULT_LEASE);

	snprintf(key, sizeof(key), "%s.plugins." CUPP_PLUGIN_NAME ".sweep_interval", lib->ns);
	this->sweep_interval = settings_get_int(key, CUPP_DEFAULT_SWEEP);

	/* fail-fast：策略加载失败 → 返回 NULL → charon 拒绝加载插件 */
	this->policy = policy_engine_create(this->policy_path);
	if (!this->policy)
	{
		cupp_err("abort plugin load: policy '%s' unavailable", this->policy_path);
		plugin_destroy_internal(this);
		return NULL;
	}
	this->allocator = ip_allocator_create(this->policy, this->pool_name,
										  this->lease_timeout);
	if (!this->allocator)
	{
		cupp_err("abort plugin load: ip_allocator_create failed");
		plugin_destroy_internal(this);
		return NULL;
	}
	this->listener = event_listener_create(this->allocator);
	if (!this->listener)
	{
		cupp_err("abort plugin load: event_listener_create failed");
		plugin_destroy_internal(this);
		return NULL;
	}

	cupp_log("plugin loaded (policy=%s pool_name=%s)",
			 this->policy_path, this->pool_name);
	return &this->public;
}
/* 修改内容：创建插件入口实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */
