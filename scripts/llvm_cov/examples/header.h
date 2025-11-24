/*
 * Copyright (C) 2025 Bayerische Motoren Werke Aktiengesellschaft (BMW AG). All rights reserved.
 */

#pragma once

int Function();

template <typename T>
T Template(T t)
{
    if (t > 0)
        return t * t;
    else
        return t;
}
