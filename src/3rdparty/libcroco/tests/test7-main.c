/* -*- Mode: C; indent-tabs-mode:nil; c-basic-offset:8 -*- */

/*
 * This file is part of The Croco Library
 *
 * Copyright (C) 2002-2003 Dodji Seketeli <dodji@seketeli.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms 
 * of version 2.1 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the 
 * GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#include <string.h>
#include "libcroco.h"
#include "cr-test-utils.h"

const guchar *gv_cssbuf =
        (const guchar *) ".exp1n1 {stroke-width:4E6}"
        ".exp1n2 {stroke-width:4e6}"
        ".exp1n3 {stroke-width:4e+6}"
        ".exp2n1 {stroke-width:4E-6}"
        ".exp2n2 {stroke-width:4e-6}"
        ".exp3n1 {stroke-width:4e6em}"
        ".exp3n2 {stroke-width:4e6ex}"
        ".exp3n3 {stroke-width:4e6in}"
        ".exp4n1 {stroke-width:3.14e4}"
        ".exp4n2 {stroke-width:3.14e-4}"
        ".e4n2 {stroke-width:.24e-4}"
        ".e4n3 {stroke-width:1.e1}";            // This one should be ignored

static enum CRStatus
  test_cr_parser_parse (void);

/**
 *The test of the cr_input_read_byte() method.
 *Reads the each byte of a_file_uri using the
 *cr_input_read_byte() method. Each byte is send to
 *stdout.
 *@param a_file_uri the file to read.
 *@return CR_OK upon successful completion of the
 *function, an error code otherwise.
 */
static enum CRStatus
test_cr_parser_parse (void)
{
        enum CRStatus status = CR_OK;
        CROMParser *parser = NULL;
        CRStyleSheet *stylesheet = NULL;

        parser = cr_om_parser_new (NULL);
        status = cr_om_parser_parse_buf (parser, (guchar *) gv_cssbuf,
                                         strlen ((const char *) gv_cssbuf),
                                         CR_ASCII, &stylesheet);

        if (status == CR_OK && stylesheet) {
                cr_stylesheet_dump (stylesheet, stdout);
                // Adding this because my test editor adds a newline character
                // at the last line, whereas cr_stylesheet_dump doesn't, which
                // results in a diff error/warning.
                printf("\n");
                cr_stylesheet_destroy (stylesheet);
        }
        cr_om_parser_destroy (parser);

        return status;
}

/**
 *The entry point of the testing routine.
 */
int
main (int argc, char **argv)
{
        enum CRStatus status = CR_OK;

        status = test_cr_parser_parse ();

        if (status != CR_OK) {
                g_print ("\nKO\n");
        }

        return 0;
}
