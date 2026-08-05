/* 修改内容：force-include 包装头，确保 config.h + _GNU_SOURCE + sys/types.h 兜底 修改人：pengjunlin 时间：2026-08-05 00:00:00 -- start ---- */
/*
 * 当 CMake 传 -include 本文件时，保证：
 *   1. 先定义 _GNU_SOURCE（让 glibc 的 <sys/types.h> 暴露 u_int/u_char）
 *   2. 再 include strongSwan 的 config.h（由 ./configure 生成）
 *   3. 最后 include <sys/types.h> 兜底，确保类型一定可用
 */
#ifndef CUPP_FORCE_INCLUDE_H_
#define CUPP_FORCE_INCLUDE_H_

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

/* strongSwan 源码树的 config.h 路径（编译时由 CMake -I 指定到源码根） */
#include "config.h"

#include <sys/types.h>

#endif /* CUPP_FORCE_INCLUDE_H_ */
/* 修改内容：force-include 包装头，确保 config.h + _GNU_SOURCE + sys/types.h 兜底 修改人：pengjunlin 时间：2026-08-05 00:00:00 -- end ---- */
