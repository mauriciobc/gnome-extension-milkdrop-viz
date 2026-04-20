import sys


def _read_next_ppm_header_line(f):
    """Next non-empty, non-comment line in PPM header (P6); comments start with #."""
    while True:
        line = f.readline()
        if not line:
            return None
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith(b'#'):
            continue
        return line.decode('ascii').strip()


def is_black(filename):
    try:
        with open(filename, 'rb') as f:
            header = f.readline().decode('ascii').strip()
            if header != 'P6':
                print(f"{filename} is not P6 PPM")
                return

            dims = _read_next_ppm_header_line(f)
            maxval = _read_next_ppm_header_line(f)
            if dims is None or maxval is None:
                print(f"{filename}: truncated PPM header")
                return

            data = f.read()

            # Check if any byte is non-zero
            if any(b != 0 for b in data):
                print(f"NOT BLACK: Image has non-zero pixels. Size: {len(data)} bytes")
            else:
                print(f"BLACK: Image is completely black. Size: {len(data)} bytes")
    except Exception as e:
        print(f"Error reading {filename}: {e}")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python check_ppm_black.py <file>", file=sys.stderr)
        sys.exit(1)
    is_black(sys.argv[1])
