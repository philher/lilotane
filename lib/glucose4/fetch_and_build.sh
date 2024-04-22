#!/bin/bash

if [ ! -d glucose-4 ]; then
    wget https://github.com/audemard/glucose/archive/refs/tags/4.1.tar.gz
    tar xzvf glucose-syrup-4.1.tgz
    mv glucose-syrup-4.1 glucose-4
    patch glucose-4/core/Solver.h < Solver.h.patch
    patch glucose-4/core/Solver.cc < Solver.cc.patch
fi
make
