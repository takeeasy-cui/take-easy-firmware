/* SPDX-License-Identifier: Apache-2.0
 * 中文：四位锁屏密码的原子NVS凭据；不存明文、不覆盖普通配置备份。
 * English: Atomic NVS credentials for a four-digit screen lock; no plaintext or ordinary-backup overwrite.
 * 用户修订（上游 rc97 对齐）：读取异常不阻塞启动且不清空原密码；可正常读取的已设密码仍需验证。
 * User revision (aligned with upstream rc97): read errors do not block startup or erase saved credentials; readable enabled PINs still require verification.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
/// 默认关闭；读取失败也按关闭返回，不缓存失败、不修改存储。/ Default off; failed reads also return off without caching faults or touching storage.
bool lock_pin_enabled(void);
/// 是否成功读取设置；不可用时禁止创建或覆盖密码。/ Whether settings were read successfully; an unavailable state cannot create or overwrite a PIN.
bool lock_pin_available(void);
/// 校验四位数字，常数时间比较摘要。/ Validate four digits with a constant-time digest comparison.
bool lock_pin_verify(const char pin[5]);
/// 未开启时old为NULL；开启时必须正确；new为NULL代表关闭。/ Null old enables from off; enabled changes require the old PIN; null new disables.
esp_err_t lock_pin_replace(const char old_pin[5], const char new_pin[5]);
/// 清零短暂密码工作区；禁止把其内容写进日志。/ Wipe temporary credentials; never log their contents.
void lock_pin_wipe(void *bytes, unsigned length);
