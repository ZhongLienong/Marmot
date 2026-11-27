"""Check that each production project uses its own source tree.

    runtime   -> runtime
    compiler  -> compiler
    driver    -> compiler, driver
    stdlib    -> stdlib, support
    web       -> no C++ components
    web_host  -> runtime
    vm        -> runtime, vm

Every quoted #include is resolved against the compiler and runtime include
roots. Includes that resolve nowhere (system and third-party headers) are
ignored. The CMake production target links are checked as well.

Inside the compiler, the pipeline (projects/marmotc/src/Compiler) compiles from the
CompilationInputs it is given and may not read environment variables. Projects
and packages are resolved by the marmot tool before the compiler is called.
"""

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from lib.host import REPO_ROOT as ROOT

INCLUDE_ROOTS = {
    'runtime': ROOT / 'projects' / 'marmotvm' / 'src',
    'compiler': ROOT / 'projects' / 'marmotc' / 'src',
    'stdlib': ROOT / 'projects' / 'marmotstdlib' / 'src',
    'support': ROOT / 'projects' / 'marmotstdlib' / 'support',
}

# The compiler's driver stays outside the compiler pipeline.
DRIVER_DIRS = [
    ROOT / 'projects' / 'marmotc' / 'src' / 'Utility' / name
    for name in ('CLI', 'Driver')
]
DRIVER_FILES = [ROOT / 'projects' / 'marmotc' / 'src' / 'Marmot.cpp']

ALLOWED = {
    'runtime': {'runtime'},
    'compiler': {'compiler'},
    'driver': {'compiler', 'driver'},
    'native_tests': set(),
    'stdlib': {'stdlib', 'support'},
    'support': {'support'},
    'web': set(),
    'web_host': {'runtime'},
    'vm': {'runtime', 'vm'},
}

INCLUDE = re.compile(r'^\s*#\s*include\s+[<"]([^>"]+)[>"]', re.MULTILINE)

PIPELINE_DIR = ROOT / 'projects' / 'marmotc' / 'src' / 'Compiler'
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
    if path == ROOT / 'projects' / 'web' / 'WasmStdlib.cpp':
        return 'web_host'
    if (ROOT / 'projects' / 'web') in path.parents:
        return 'web'
    for name, include_root in INCLUDE_ROOTS.items():
        if include_root in path.parents:
            return name
    for project, component in [('marmotc', 'driver'), ('marmotvm', 'runtime'), ('marmotstdlib', 'stdlib')]:
        if (ROOT / 'projects' / project / 'tests') in path.parents:
            return component
    if (ROOT / 'test' / 'integration' / 'native') in path.parents:
        return 'native_tests'
    if (ROOT / 'test' / 'integration') in path.parents:
        return 'driver'
    return None


def resolve(including_file, target):
    local = (including_file.parent / target)
    if local.is_file():
        return local
    source_component = component_of(including_file)
    include_roots = sorted(INCLUDE_ROOTS.items(), key=lambda item: item[0] not in ALLOWED[source_component])
    for _, include_root in include_roots:
        candidate = include_root / target
        if candidate.is_file():
            return candidate
    return None


def main(argv: list[str]) -> int:
    sources = []
    for folder in (ROOT / 'projects' / 'marmotvm' / 'src', ROOT / 'projects' / 'marmotc' / 'src', ROOT / 'projects' / 'marmotstdlib' / 'src', ROOT / 'projects' / 'marmotstdlib' / 'support', ROOT / 'projects' / 'web', ROOT / 'test' / 'integration', ROOT / 'projects' / 'marmotc' / 'tests', ROOT / 'projects' / 'marmotvm' / 'tests'):
        for pattern in ('*.h', '*.cpp', '*.def'):
            sources.extend(folder.rglob(pattern))

    violations = []
    for filename in ('CMakeLists.txt', 'CMakePresets.json'):
        if (ROOT / filename).exists():
            violations.append(f'{filename}: C++ builds must belong to individual projects')
    for source in sorted(sources):
        source_component = component_of(source)
        if source_component is None:
            continue
        text = source.read_text(encoding='utf-8', errors='replace')
        violations.extend(pipeline_violations(source, text))
        relative = source.relative_to(ROOT).as_posix()
        for match in INCLUDE.finditer(text):
            header = match.group(1)
            resolved = resolve(source, header)
            if resolved is None:
                continue
            target_component = component_of(resolved)
            if target_component in ALLOWED[source_component]:
                continue
            if target_component is None and not resolved.resolve().is_relative_to(ROOT.resolve()):
                continue
            line = text.count('\n', 0, match.start()) + 1
            violations.append(
                f'{relative}:{line}: {source_component} includes '
                f'{target_component or "unowned repository"} header "{header}"'
            )

    projects = ('marmotc', 'marmotvm', 'marmotstdlib')
    production = {}
    for project in projects:
        directory = ROOT / 'projects' / project
        files = [directory / 'CMakeLists.txt', *directory.joinpath('cmake').glob('*.cmake')]
        production[project] = '\n'.join(path.read_text(encoding='utf-8') for path in files
                                        if path.name != 'UnitTests.cmake')
    cmake = '\n'.join(production.values())
    if re.search(r'add_library\(\s*MarmotBytecode\b', cmake):
        violations.append('CMakeLists.txt: MarmotBytecode remains a shared target')
    targets = {target: project for project, source in production.items()
               for target in re.findall(r'\badd_(?:library|executable)\(\s*(\w+)', source)}
    for project, source in production.items():
        sibling_path = r'(?:\.\./|projects/)(?:' + '|'.join(set(projects) - {project}) + r')(?=[/\s"])'
        if re.search(sibling_path, source):
            violations.append(f'{project}: production CMake references a sibling source tree')
        for link in re.finditer(r'\btarget_link_libraries\(\s*(\w+)\s+([^)]*)\)', source):
            for dependency in re.findall(r'\b\w+\b', link.group(2)):
                if targets.get(dependency, project) != project or dependency in {'MarmotNativeSupport', 'MarmotBytecode'}:
                    violations.append(f'{project}: {link.group(1)} links {dependency}')

    web = ROOT / 'projects' / 'web'
    if any(web.rglob('CMakeLists.txt')) or any(web.rglob('*.cmake')):
        violations.append('projects/web: browser integration must orchestrate independent builds in JavaScript')

    if violations:
        print('Layering violations:')
        for violation in violations:
            print('  ' + violation)
        return 1
    print(f'Layering OK: {len(sources)} files, no forbidden includes.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
