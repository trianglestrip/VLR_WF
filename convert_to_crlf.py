#!/usr/bin/env python3
"""
Convert all text files to CRLF line endings
"""
import os
import sys
from pathlib import Path

def convert_to_crlf(file_path):
    """Convert a file to CRLF line endings"""
    try:
        # Read file in binary mode
        with open(file_path, 'rb') as f:
            content = f.read()
        
        # Check if file has any LF
        if b'\r\n' in content or b'\n' not in content:
            # Already CRLF or no line endings
            return False
        
        # Convert LF to CRLF
        content = content.replace(b'\r\n', b'\n')  # Normalize first
        content = content.replace(b'\n', b'\r\n')  # Convert to CRLF
        
        # Write back
        with open(file_path, 'wb') as f:
            f.write(content)
        
        return True
    except Exception as e:
        print(f"Error processing {file_path}: {e}")
        return False

def main():
    # File extensions to process
    extensions = {
        '.cpp', '.h', '.cu', '.cuh', '.c',
        '.md', '.txt', '.cmake', '.bat',
        '.hpp', '.cc', '.cxx', '.hxx'
    }
    
    # Directories to skip
    skip_dirs = {
        '.git', 'build', 'build2', 'bin', '.vs',
        '__pycache__', 'node_modules'
    }
    
    root = Path('.')
    converted_count = 0
    skipped_count = 0
    
    print("Converting files to CRLF...")
    print("-" * 60)
    
    for file_path in root.rglob('*'):
        # Skip directories
        if file_path.is_dir():
            continue
        
        # Skip if in excluded directory
        if any(skip_dir in file_path.parts for skip_dir in skip_dirs):
            continue
        
        # Check extension
        if file_path.suffix.lower() not in extensions:
            # Also check for CMakeLists.txt
            if file_path.name != 'CMakeLists.txt':
                continue
        
        # Convert
        if convert_to_crlf(file_path):
            print(f"Converted: {file_path}")
            converted_count += 1
        else:
            skipped_count += 1
    
    print("-" * 60)
    print(f"Converted: {converted_count} files")
    print(f"Skipped: {skipped_count} files (already CRLF or no LF)")
    print("Done!")

if __name__ == '__main__':
    main()
