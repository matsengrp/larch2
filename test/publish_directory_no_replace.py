#!/usr/bin/env python3.14
"""Atomically publish a directory without replacing any existing path."""

from __future__ import annotations

import argparse
import ctypes
import errno
import os
import pathlib
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("staging", type=pathlib.Path)
    parser.add_argument("destination", type=pathlib.Path)
    arguments = parser.parse_args()
    staging = arguments.staging.absolute()
    destination = arguments.destination.absolute()
    if not staging.is_dir() or staging.is_symlink():
        raise RuntimeError("staging must be a direct directory")
    if staging.parent != destination.parent:
        raise RuntimeError("staging and destination must share a parent")

    try:
        libc = ctypes.CDLL(None, use_errno=True)
        renameat2 = libc.renameat2
    except (AttributeError, OSError) as error:
        raise RuntimeError("renameat2 is unavailable") from error
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                          ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    result = renameat2(-100, os.fsencode(staging), -100,
                       os.fsencode(destination), 1)  # RENAME_NOREPLACE
    if result == 0:
        return 0
    error = ctypes.get_errno()
    if error == errno.EEXIST:
        raise RuntimeError(f"destination already exists: {destination}")
    if error in {errno.EINVAL, errno.ENOSYS, errno.EOPNOTSUPP}:
        # Some compatibility filesystems (notably ZFS through Linuxulator)
        # reject RENAME_NOREPLACE.  A same-directory relative symlink is still
        # one atomic, no-replace namespace operation.  The payload retains its
        # staging name and is reachable only after the complete link appears.
        try:
            os.symlink(staging.name, destination, target_is_directory=True)
        except FileExistsError as exists:
            raise RuntimeError(
                f"destination already exists: {destination}") from exists
        return 0
    raise RuntimeError(
        f"atomic no-replace publication failed: {os.strerror(error)}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"publication_error: {error}", file=sys.stderr)
        raise SystemExit(1)
