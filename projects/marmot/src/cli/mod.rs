mod compile;
mod dispatch;
mod fmt;
mod init;
mod options;
mod packages;
mod test;

pub(crate) use dispatch::execute;
use options::Options;
pub(crate) use options::{CommandKind, USAGE, parse_options};

fn print_warnings(warnings: &[String]) {
    for warning in warnings {
        eprintln!("[packages] {warning}");
    }
}
