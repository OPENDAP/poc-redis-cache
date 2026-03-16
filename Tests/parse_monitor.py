#!/usr/bin/env python3
"""
CLI program to extract monitor data from simulator log and output as CSV.

Usage: python3 parse_monitor.py [input_file] [output_file]

If output_file is not provided, outputs to stdout.
If input_file is not provided, defaults to simulator_0.log in the same directory.
"""

import sys
import os
import csv
import re

def parse_monitor_line(line):
    """Parse a monitor line and return t, bytes and keys."""
    match = re.search(r'\[monitor t=(\d+)s\] total_bytes=(\d+) keys=(\d+)', line)
    if match:
        return int(match.group(1)), int(match.group(2)), int(match.group(3))
    return None

def main():
    if len(sys.argv) > 3:
        print("Usage: python3 parse_monitor.py [input_file] [output_file]", file=sys.stderr)
        sys.exit(1)

    # Default input file
    input_file = 'simulator_0.log'
    if len(sys.argv) >= 2:
        input_file = sys.argv[1]

    # Check if input file exists
    if not os.path.isfile(input_file):
        print(f"Error: Input file '{input_file}' not found.", file=sys.stderr)
        sys.exit(1)

    # Output file or stdout
    output_file = None
    if len(sys.argv) == 3:
        output_file = sys.argv[2]

    # Read the file
    data = []
    with open(input_file, 'r') as f:
        for line in f:
            if line.startswith('[monitor t='):
                result = parse_monitor_line(line)
                if result:
                    data.append(result)

    # Write CSV
    writer = csv.writer(sys.stdout if output_file is None else open(output_file, 'w', newline=''))
    writer.writerow(['t', 'bytes', 'keys'])
    for t_val, bytes_val, keys_val in data:
        writer.writerow([t_val, bytes_val, keys_val])

if __name__ == '__main__':
    main()