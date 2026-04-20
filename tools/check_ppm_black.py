import sys

def is_black(filename):
    try:
        with open(filename, 'rb') as f:
            header = f.readline().decode('ascii').strip()
            if header != 'P6':
                print(f"{filename} is not P6 PPM")
                return
            
            # Skip comments
            while True:
                pos = f.tell()
                line = f.readline()
                if not line.startswith(b'#'):
                    f.seek(pos)
                    break
                    
            dims = f.readline().decode('ascii').strip()
            maxval = f.readline().decode('ascii').strip()
            
            data = f.read()
            
            # Check if any byte is non-zero
            if any(b != 0 for b in data):
                print(f"NOT BLACK: Image has non-zero pixels. Size: {len(data)} bytes")
            else:
                print(f"BLACK: Image is completely black. Size: {len(data)} bytes")
    except Exception as e:
        print(f"Error reading {filename}: {e}")

if __name__ == '__main__':
    is_black(sys.argv[1])
