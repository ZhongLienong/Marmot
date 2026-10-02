use super::newest_build;
use crate::test_support::TempTree;

#[test]
fn the_checkout_build_is_the_one_whose_compiler_or_vm_was_written_last() {
    let suffix = std::env::consts::EXE_SUFFIX;
    let tree = TempTree::new(&[
        (&format!("marmotc/dev/out/marmotc{suffix}"), ""),
        (&format!("marmotvm/dev/out/marmotvm{suffix}"), ""),
        (&format!("marmotc/release/out/marmotc{suffix}"), ""),
        (&format!("marmotvm/release/out/marmotvm{suffix}"), ""),
    ]);
    let stamp = |relative: &str, seconds: u64| {
        std::fs::File::options()
            .write(true)
            .open(tree.path(&format!("{relative}{suffix}")))
            .unwrap()
            .set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_secs(seconds))
            .unwrap();
    };

    // A change to the VM relinks only marmotvm: that build is still the newest,
    // though its compiler is older than the other build's.
    stamp("marmotc/dev/out/marmotc", 100);
    stamp("marmotvm/dev/out/marmotvm", 300);
    stamp("marmotc/release/out/marmotc", 200);
    stamp("marmotvm/release/out/marmotvm", 200);
    assert_eq!(
        newest_build(&tree.0),
        Some(tree.path(&format!("marmotc/dev/out/marmotc{suffix}")))
    );

    stamp("marmotc/release/out/marmotc", 400);
    assert_eq!(
        newest_build(&tree.0),
        Some(tree.path(&format!("marmotc/release/out/marmotc{suffix}")))
    );
}
