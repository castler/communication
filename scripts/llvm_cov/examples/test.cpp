// Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.

#include "scripts/llvm_cov/examples/header.h"
#include "scripts/llvm_cov/examples/package/source.h"

int main()
{
    Function();
    package::Function();

    Function();

    Template(-1);
    Template(1.0);

    return 0;
}
