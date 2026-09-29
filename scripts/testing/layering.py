"""Check that the C++ compiler and VM use only their own code.

    runtime   -> runtime
    compiler  -> compiler
    driver    -> compiler, driver
    web       -> runtime, compiler
    vm        -> runtime, vm

Every quoted #include is resolved against the compiler and runtime include
roots. Includes that resolve nowhere (system and third-party headers) are
ignored. The CMake production target links are checked as well.

Inside the compiler, the pipeline (compiler/src/Compiler) compiles from the
CompilationInputs it is given and may not read environment variables. Projects
and packages are resolved by the marmot tool before the compiler is called.
"""

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT as ROOT

INCLUDE_ROOTS = {
    'runtime': ROOT / 'runtime' / 'src',
    'compiler': ROOT / 'compiler' / 'src',
}

# The folders of compiler/src that form the driver, not the compiler library.
DRIVER_DIRS = [
    ROOT / 'compiler' / 'src' / 'Utility' / name
    for name in ('CLI', 'Driver')
]
DRIVER_FILES = [ROOT / 'compiler' / 'src' / 'Marmot.cpp']

ALLOWED = {
    'runtime': {'runtime'},
    'compiler': {'compiler'},
    'driver': {'compiler', 'driver'},
    'web': {'runtime', 'compiler'},
    'vm': {'runtime', 'vm'},
}

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)

PIPELINE_DIR = ROOT / 'compiler' / 'src' / 'Compiler'
ENVIRONMENT_READ = re.compile(r'\b(getenv|_dupenv_s|_wgetenv|secure_getenv)\s*\(')


def pipeline_violations(source, text):
    """The compiler pipeline takes its inputs; it does not go looking for them."""
    if PIPELINE_DIR not in source.resolve().parents:
        return []

    violations = []
    relative = source.relative_to(ROOT).as_posix()
    for match in ENVIRONMENT_READ.finditer(text):
        line = text.count('\n', 0, match.start()) + 1
        violations.append(f'{relative}:{line}: the compiler pipeline reads the environment ({match.group(1)})')
    return violations


def component_of(path):
    path = path.resolve()
    if path in DRIVER_FILES or any(directory in path.parents for directory in DRIVER_DIRS):
        return 'driver'
    if (ROOT / 'web' / 'src') in path.parents:
        return 'web'
    if (ROOT / 'vm' / 'src') in path.parents:
        return 'vm'
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


def main(argv: list[str]) -> int:
    sources = []
    for folder in (ROOT / 'runtime' / 'src', ROOT / 'compiler' / 'src', ROOT / 'web' / 'src', ROOT / 'vm' / 'src'):
        for pattern in ('*.h', '*.cpp', '*.def'):
            sources.extend(folder.rglob(pattern))

    violations = []
    for source in sorted(sources):
        source_component = component_of(source)
        if source_component is None:
            continue
        text = source.read_text(encoding='utf-8', errors='replace')
        violations.extend(pipeline_violations(source, text))
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

    cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
    if re.search(r'add_library\(\s*MarmotBytecode\b', cmake):
        violations.append('CMakeLists.txt: MarmotBytecode remains a shared target')
    links = {}
    for match in re.finditer(r'target_link_libraries\(\s*(\w+)\s+([^)]*)\)', cmake, re.DOTALL):
        links.setdefault(match.group(1), set()).update(
            token for token in match.group(2).split()
            if token not in {'PUBLIC', 'PRIVATE', 'INTERFACE'}
        )

    def reachable(target: str, visited: set[str] | None = None) -> set[str]:
        visited = set() if visited is None else visited
        for dependency in links.get(target, set()) - visited:
            visited.add(dependency)
            visited.update(reachable(dependency, visited))
        return visited

    for target, forbidden in (
        ('marmotc', {'MarmotRuntime', 'MarmotBytecode'}),
        ('marmotvm', {'MarmotCompiler', 'MarmotDriver', 'MarmotBytecode'}),
    ):
        for dependency in reachable(target) & forbidden:
            violations.append(f'CMakeLists.txt: {target} links {dependency}')

    if violations:
        print('Layering violations:')
        for violation in violations:
            print('  ' + violation)
        return 1
    print(f'Layering OK: {len(sources)} files, no forbidden includes.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
