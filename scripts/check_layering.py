"""Check that the C++ components include only what they are allowed to.

    common    -> common
    runtime   -> common, runtime
    compiler  -> common, compiler
    driver    -> common, runtime, compiler, driver   (the application layer)
    web       -> common, runtime, compiler

Every quoted #include is resolved against the three include roots
(common/src, runtime/src, compiler/src). Includes that resolve nowhere (system
and third-party headers) are ignored. Exit status 1 lists each forbidden edge.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

INCLUDE_ROOTS = {
    'common': ROOT / 'common' / 'src',
    'runtime': ROOT / 'runtime' / 'src',
    'compiler': ROOT / 'compiler' / 'src',
}

# The folders of compiler/src that form the driver, not the compiler library.
DRIVER_DIRS = [
    ROOT / 'compiler' / 'src' / 'Utility' / name
    for name in ('CLI', 'Driver', 'TestRunner', 'OutputCapture')
]
DRIVER_FILES = [ROOT / 'compiler' / 'src' / 'Marmot.cpp']

ALLOWED = {
    'common': {'common'},
    'runtime': {'common', 'runtime'},
    'compiler': {'common', 'compiler'},
    'driver': {'common', 'runtime', 'compiler', 'driver'},
    'web': {'common', 'runtime', 'compiler'},
}

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def component_of(path):
    path = path.resolve()
    if path in DRIVER_FILES or any(directory in path.parents for directory in DRIVER_DIRS):
        return 'driver'
    if (ROOT / 'web' / 'src') in path.parents:
        return 'web'
    for name, include_root in INCLUDE_ROOTS.items():
        if include_root in path.parents:
            return name
    return None


def resolve(including_file, target):
    local = (including_file.parent / target)
    if local.is_file():
        return local
    for include_root in INCLUDE_ROOTS.values():
        candidate = include_root / target
        if candidate.is_file():
            return candidate
    return None


def main():
    sources = []
    for folder in (ROOT / 'common' / 'src', ROOT / 'runtime' / 'src', ROOT / 'compiler' / 'src', ROOT / 'web' / 'src'):
        for pattern in ('*.h', '*.cpp', '*.def'):
            sources.extend(folder.rglob(pattern))

    violations = []
    for source in sorted(sources):
        source_component = component_of(source)
        if source_component is None:
            continue
        text = source.read_text(encoding='utf-8', errors='replace')
        for match in INCLUDE.finditer(text):
            resolved = resolve(source, match.group(1))
            if resolved is None:
                continue
            target_component = component_of(resolved)
            if target_component is None or target_component in ALLOWED[source_component]:
                continue
            line = text.count('\n', 0, match.start()) + 1
            violations.append(
                f'{source.relative_to(ROOT).as_posix()}:{line}: {source_component} includes '
                f'{target_component} header "{match.group(1)}"'
            )

    if violations:
        print('Layering violations:')
        for violation in violations:
            print('  ' + violation)
        return 1
    print(f'Layering OK: {len(sources)} files, no forbidden includes.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
