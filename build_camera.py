#!/usr/bin/env python3
"""Build the camera receiver using the installed GigE-V C example makefile."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-example', required=True, type=Path,
                        help='Installed examples/genicam_c_demo or examples3/genicam_c_demo directory')
    parser.add_argument('--build-dir', type=Path,
                        help='New sibling directory; defaults to plif_camera alongside the SDK example')
    args = parser.parse_args()
    source = args.sdk_example.expanduser().resolve()
    target = (args.build_dir.expanduser().resolve() if args.build_dir else source.parent / 'plif_camera')
    repo = Path(__file__).resolve().parent
    if not (source / 'genicam_c_demo.c').is_file():
        parser.error('The SDK example directory must contain genicam_c_demo.c.')
    if not any((source / name).is_file() for name in ('Makefile', 'makefile', 'GNUmakefile')):
        parser.error('The SDK example makefile is missing.')
    if target.parent != source.parent:
        parser.error('Use a sibling of the SDK example so its relative include/library paths remain valid.')
    if target.exists():
        parser.error(f'{target} already exists. Choose a new --build-dir; existing files are not overwritten.')
    if shutil.which('make') is None:
        parser.error('Install make and the C compiler before building.')
    shutil.copytree(source, target, symlinks=True)
    # Keep all local SDK dependencies in place, but compile the receiver as the example target.
    for name in ('genicam_c_demo.c', 'genicam_c_demo', 'genicam_c_demo.o'):
        path = target / name
        if path.is_symlink() or path.is_file():
            path.unlink()
    shutil.copyfile(repo / 'server.c', target / 'genicam_c_demo.c')
    try:
        # -B prevents a copied executable/object from being mistaken for a fresh build.
        subprocess.run(['make', '-B', 'genicam_c_demo'], cwd=target, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f'Build failed: {exc}', file=sys.stderr)
        print(f'Working copy left at {target}. Check SDK setup, architecture and TIFF support.', file=sys.stderr)
        return 1
    executable = target / 'genicam_c_demo'
    if not executable.is_file():
        print('The SDK makefile did not create genicam_c_demo.', file=sys.stderr)
        return 1
    destination = repo / 'build' / 'plif_camera'
    destination.parent.mkdir(exist_ok=True)
    shutil.copy2(executable, destination)
    print(f'Built {destination}')
    print(f'Run: {destination} --help')
    return 0


if __name__ == '__main__':
    sys.exit(main())
