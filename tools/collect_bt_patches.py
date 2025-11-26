#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2025 The Pybricks Authors

"""
Tool to collect bluetooth patch files in the user's cache directory.
"""

import argparse
import os
import re
import subprocess
import shutil
from pathlib import Path

# Destination directory in user's cache directory
DEST_DIR = Path.home() / ".cache" / "pybricks" / "virtualhub" / "bt_firmware"


def sparse_checkout(subdir: str, repo_url: str, paths: list[str], branch: str = "master"):
    """
    Perform sparse checkout of specified paths from a git repository.
    
    Args:
        subdir: Subdirectory name under DEST_DIR
        repo_url: URL of the git repository
        paths: List of paths to checkout (e.g., ['brcm/', 'rtl_bt/'])
        branch: Git branch to pull from (default: 'master')
    """
    checkout_dir = DEST_DIR / subdir
    git_dir = checkout_dir / ".git"
    
    # Check if repo already exists
    if git_dir.exists():
        # Just pull the latest changes
        subprocess.run(["git", "pull", "origin", branch], cwd=checkout_dir, check=True)
        return
    
    # Create the directory
    checkout_dir.mkdir(parents=True, exist_ok=True)
    
    # Initialize git repo
    subprocess.run(["git", "init"], cwd=checkout_dir, check=True)
    
    # Add remote
    subprocess.run(["git", "remote", "add", "origin", repo_url], cwd=checkout_dir, check=True)
    
    # Enable sparse checkout
    subprocess.run(["git", "config", "core.sparseCheckout", "true"], cwd=checkout_dir, check=True)
    
    # Specify the paths to checkout
    sparse_checkout_file = checkout_dir / ".git" / "info" / "sparse-checkout"
    sparse_checkout_file.parent.mkdir(parents=True, exist_ok=True)
    sparse_checkout_file.write_text("\n".join(paths) + "\n")
    
    # Pull the files
    subprocess.run(["git", "pull", "origin", branch], cwd=checkout_dir, check=True)


def collect_firmware(subdir: str, pattern: str):
    """
    Create symbolic links for firmware files in DEST_DIR.
    
    Args:
        subdir: Subdirectory under DEST_DIR containing firmware files
        pattern: Regex pattern to match and optionally extract parts for renaming.
                 - If pattern has 2 capture groups, uses them concatenated as the link name
                 - If pattern has no capture groups, uses original filename for matches
    """
    firmware_dir = DEST_DIR / subdir
    
    if not firmware_dir.exists():
        return
    
    # Compile pattern
    regex = re.compile(pattern)
    
    for firmware_file in firmware_dir.iterdir():
        if not firmware_file.is_file():
            continue
        
        # Check if filename matches pattern
        match = regex.match(firmware_file.name)
        if not match:
            continue
        
        # Determine link name based on capture groups
        groups = match.groups()
        if len(groups) == 2:
            # Two groups: concatenate them for the link name
            link_name = DEST_DIR / "".join(groups)
        else:
            # No groups: use original filename
            link_name = DEST_DIR / firmware_file.name
        
        # Skip if link already exists
        if link_name.exists() or link_name.is_symlink():
            continue
        
        # Create the symbolic link
        link_name.symlink_to(firmware_file)


def main():
    """Main entry point for collecting bluetooth patch files."""
    parser = argparse.ArgumentParser(description="Collect bluetooth patch files in the user's cache directory")
    parser.add_argument("--clean", action="store_true", help="Delete the entire destination directory before collecting")
    args = parser.parse_args()

    # Clean destination directory if requested
    if args.clean and DEST_DIR.exists():
        shutil.rmtree(DEST_DIR)

    # Checkout brcm directory from broadcom-bt-firmware repo
    sparse_checkout(
        subdir="brcm",
        repo_url="https://github.com/winterheart/broadcom-bt-firmware",
        paths=["brcm/"],
        branch="master"
    )

    # Checkout rtl_bt and intel directories from linux-firmware repo
    sparse_checkout(
        subdir="linux_firmware",
        repo_url="https://gitlab.com/kernel-firmware/linux-firmware.git",
        paths=["rtl_bt/", "intel/"],
        branch="main",
    )

    # Collect firmware files into a single directory. Rename the brcm firmware
    # files to match btstack's filename expectations.
    collect_firmware("brcm/brcm", r'([^-]+)[^.]*(\..+)')
    collect_firmware("linux_firmware/intel", r"^ibt.*(?:(ddc|sfi))$")
    collect_firmware("linux_firmware/rtl_bt", r'^.*\.bin$')


if __name__ == "__main__":
    main()
