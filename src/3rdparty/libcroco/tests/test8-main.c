/* -*- Mode: C; indent-tabs-mode:nil; c-basic-offset:8 -*- */

/*
 * This file is part of The Croco Library
 *
 * Copyright (C) 2022 Thomas Holder
 *
 * SPDX-License-Identifier: GPL-2.1-or-later
 */

#include "libcroco.h"
#include <stdio.h>

int
main (int argc, char **argv)
{
        unsigned i = 0;
        CRSelector *selector;

        char const *selector_strings[] = {
            // must not have leading whitespace!?
            "foo", "foo,bar", "foo , bar ", "foo > bar", ".foo .bar",
        };

        for (i = 0; i < G_N_ELEMENTS (selector_strings); ++i) {
                printf ("****************\n");
                printf ("Parsing '%s'\n", selector_strings[i]);

                selector = cr_selector_parse_from_buf (
                    (guchar const *) selector_strings[i], CR_UTF_8);

                if (!selector) {
                        printf ("is NULL\n");
                } else {
                        cr_selector_dump (selector, stdout);
                        cr_selector_unref (selector);
                        printf ("\n");
                }
        }

        return 0;
}

// vi:sw=8:expandtab
