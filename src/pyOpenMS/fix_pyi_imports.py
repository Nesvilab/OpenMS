import sys


if __name__ == '__main__':
    args = sys.argv[1:]
    with open(args[0], 'r') as f:
        save = f.readlines()
    save.insert(1, 'from pyopenms import *  # pylint: disable=wildcard-import; lgtm(py/polluting-import)')
    with open(args[0], 'w') as f:
        f.writelines(save)