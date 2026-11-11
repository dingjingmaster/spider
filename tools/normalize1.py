#!/usr/bin/env python
# -*- coding=utf-8 -*-
import os
import sys

from common import traditional_to_simplified


def walk_dir (dir):
    with os.scandir(dir) as eee:
        for e in eee:
            if e.is_file(follow_symlinks=False):
                print("%s --> %s" % (e.path, traditional_to_simplified(e.path)))
            elif e.is_dir(follow_symlinks=False):
                walk_dir(e.path)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print ("Usage: %s /root/path" % sys.argv[0])
        sys.exit(-1)
    walk_dir(sys.argv[1])
    sys.exit (0)


