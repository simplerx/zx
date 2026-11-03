//! zx 扩展的宿主侧逻辑：把项目内的 bin/zxls（Windows 为 zxls.exe）注册为
//! Zx 语言的语言服务器。
//!
//! Zed 的 settings.json 无法凭空创建 LSP adapter（只能覆盖已注册 adapter 的
//! 二进制路径），所以 LSP 必须由扩展提供。这里只返回 worktree 下的本地
//! 二进制，不做任何下载——版本跟着仓库走，zxls 与 zxc 永远同源。
//!
//! 注意：路径按宿主平台挑后缀。写死 `zxls.exe` 会让 macOS / Linux 上
//! "能高亮、就是没有补全和诊断"——文件找不到时 Zed 只在日志里记一行，
//! 界面上什么都不说。

use zed_extension_api::{self as zed, Command, LanguageServerId, Result};

struct ZxExtension;

impl zed::Extension for ZxExtension {
    fn new() -> Self {
        ZxExtension
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<Command> {
        let exe = match zed::current_platform().0 {
            zed::Os::Windows => "zxls.exe",
            _ => "zxls",
        };
        Ok(Command {
            command: format!("{}/bin/{}", worktree.root_path(), exe),
            args: vec![],
            env: vec![],
        })
    }
}

zed::register_extension!(ZxExtension);
