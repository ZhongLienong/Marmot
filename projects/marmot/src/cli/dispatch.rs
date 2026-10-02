use super::{CommandKind, Options, compile, fmt, init, packages, test};
use crate::execution::toolchain;
use std::process::ExitCode;

pub(crate) fn execute(kind: CommandKind, options: Options) -> Result<ExitCode, String> {
    if kind == CommandKind::Init {
        return Ok(init::execute(&options));
    }

    let compiler = toolchain::find_compiler(options.marmotc.as_deref());
    if kind == CommandKind::Fmt {
        return fmt::execute(&options, &compiler);
    }

    let version = toolchain::compiler_version(&compiler)?;
    match kind {
        CommandKind::Run | CommandKind::Check | CommandKind::Build | CommandKind::Plan => {
            compile::execute(kind, &options, &compiler, &version)
        }
        CommandKind::Install => packages::install(&options, &version).map(|()| ExitCode::SUCCESS),
        CommandKind::Update => packages::update(&options, &version).map(|()| ExitCode::SUCCESS),
        CommandKind::Remove => packages::remove(&options, &version).map(|()| ExitCode::SUCCESS),
        CommandKind::List => packages::list(&version).map(|()| ExitCode::SUCCESS),
        CommandKind::Test => test::execute(&options, &compiler, &version),
        CommandKind::Fmt | CommandKind::Init => unreachable!("handled above"),
    }
}
