#!/bin/sh
# Host (PC) tests: framebuffer conversion + PC tools. Run from the repo root.
set -e
mkdir -p tests/host/out
${CXX:-g++} -std=gnu++11 -Wall -Wextra -Werror -O2 -I Includes tests/host/test_capture.cpp -o tests/host/out/test_capture
tests/host/out/test_capture
python3 tests/host/check_raw.py
