// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * TODO: insert short description here
 *//*
 * Authors: see git history
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include <algorithm>

#include <glibmm/convert.h>
#include <glibmm/miscutils.h>
#include <glibmm/uriutils.h>
#include <glibmm/utility.h>

#include "../document.h"  /* Unfortunately there's a separate xml/document.h. */
#include "streq.h"

#include "io/dir-util.h"
#include "io/sys.h"

#include "object/sp-object.h"
#include "object/uri.h"

#include "xml/node.h"
#include "xml/repr.h"
#include "xml/rebase-hrefs.h"
#include "xml/href-attribute-helper.h"

using Inkscape::XML::AttributeRecord;
using Inkscape::XML::AttributeVector;

/**
 * Determine if a href needs rebasing.
 */
static bool href_needs_rebasing(char const *href)
{
    // RFC 3986 defines empty string relative URL as referring to the
    // containing document, rather than referring to the base URI.
    if (!href[0] || href[0] == '#') {
        return false;
    }

    // skip document-local queries
    if (href[0] == '?') {
        return false;
    }

    // skip absolute-path and network-path references
    if (href[0] == '/') {
        return false;
    }

    // Don't change non-file URIs (like data or http)
    auto scheme = Glib::make_unique_ptr_gfree(g_uri_parse_scheme(href));
    return !scheme || g_str_equal(scheme.get(), "file");
}

AttributeVector
Inkscape::XML::rebase_href_attrs(gchar const *const old_abs_base,
                                 gchar const *const new_abs_base,
                                 const AttributeVector &attributes)
{
    auto ret = attributes; // copy
    auto const rebase = compute_href_rebase(old_abs_base, new_abs_base, attributes);
    auto const replace_first = [&ret](GQuark key, std::string const &value) {
        auto it = std::find_if(ret.begin(), ret.end(), [key](auto const &attr) { return attr.key == key; });
        if (it != ret.end()) {
            it->value = Inkscape::Util::share_string(value.c_str());
        }
    };
    if (rebase.absref_key) {
        replace_first(rebase.absref_key, rebase.absref);
    }
    if (rebase.href_key) {
        replace_first(rebase.href_key, rebase.href);
    }
    return ret;
}

Inkscape::XML::HrefRebase
Inkscape::XML::compute_href_rebase(gchar const *const old_abs_base,
                                   gchar const *const new_abs_base,
                                   const AttributeVector &attributes)
{
    HrefRebase result;

    if (old_abs_base == new_abs_base) {
        return result;
    }

    static GQuark const href_key = g_quark_from_static_string("href");
    static GQuark const xlink_href_key = g_quark_from_static_string("xlink:href");
    static GQuark const absref_key = g_quark_from_static_string("sodipodi:absref");

    auto const find_record = [&attributes](GQuark const key) {
        return find_if(attributes.begin(), attributes.end(), [key](auto const &attr) { return attr.key == key; });
    };

    auto href_it = find_record(href_key);
    if (href_it == attributes.end()) {
        href_it = find_record(xlink_href_key);
    }
    if (href_it == attributes.end() || !href_needs_rebasing(href_it->value.pointer())) {
        return result;
    }

    auto uri = URI::from_href_and_basedir(href_it->value.pointer(), old_abs_base);
    if (!uri.getPath()) {
        // URI parsing failed
        return result;
    }

    std::string abs_href;
    try {
        abs_href = uri.toNativeFilename();
    } catch (Glib::ConvertError const &) {
        return result;
    }

    auto absref_it = find_record(absref_key);
    if (absref_it != attributes.end()) {
        if (g_file_test(abs_href.c_str(), G_FILE_TEST_EXISTS)) {
            if (!streq(abs_href.c_str(), absref_it->value.pointer())) {
                result.absref_key = absref_key;
                result.absref = abs_href;
            }
        } else if (g_file_test(absref_it->value.pointer(), G_FILE_TEST_EXISTS)) {
            uri = URI::from_native_filename(absref_it->value.pointer());
        }
    }

    std::string baseuri;
    if (new_abs_base && new_abs_base[0]) {
        baseuri = URI::from_dirname(new_abs_base).str();
    }

    auto new_href = uri.str(baseuri.c_str());
    result.href_key = href_it->key;
    result.href = new_href;

    return result;
}

static void rebase_image_href(Inkscape::XML::Node *ir, std::string const &old_base_url_str, std::string const &new_base_url_str, bool const spns) {
    
    using Inkscape::URI;

    auto [href_key, href_cstr] = Inkscape::getHrefAttribute(*ir);
    if (!href_cstr) {
        return;
    }

    if (!href_needs_rebasing(href_cstr)) {
        return;
    }

    // make absolute
    URI url;
    try {
        url = URI(href_cstr, old_base_url_str.c_str());
    } catch (...) {
        return;
    }

    // skip non-file URLs
    if (!url.hasScheme("file")) {
        return;
    }

    std::string filename;
    try {
        filename = url.toNativeFilename();
    } catch (Glib::ConvertError const &) {
        return;
    }

    // if path doesn't exist, use sodipodi:absref
    if (!g_file_test(filename.c_str(), G_FILE_TEST_EXISTS)) {
        auto spabsref = ir->attribute("sodipodi:absref");
        if (spabsref && g_file_test(spabsref, G_FILE_TEST_EXISTS)) {
            url = URI::from_native_filename(spabsref);
        }
    } else if (spns) {
        ir->setAttributeOrRemoveIfEmpty("sodipodi:absref", filename);
    }

    if (!spns) {
        ir->removeAttribute("sodipodi:absref");
    }

    auto href_str = url.str(new_base_url_str.c_str());
    href_str = Inkscape::uri_to_iri(href_str.c_str());

    ir->setAttribute(href_key, href_str);
}

void Inkscape::XML::rebase_hrefs(Inkscape::XML::Node *rootxml, gchar const *const old_base, gchar const *const new_base, bool const spns)
{
    static GQuark const code_svg_image = g_quark_from_static_string("svg:image");
    static GQuark const code_svg_use = g_quark_from_static_string("svg:use");

    using Inkscape::URI;

    std::string old_base_url_str = URI::from_dirname(old_base).str();
    std::string new_base_url_str;

    if (new_base) {
        new_base_url_str = URI::from_dirname(new_base).str();
    }
    sp_repr_visit_descendants(rootxml, [&](Inkscape::XML::Node *ir) {
        if (ir->code() == code_svg_image) {
            rebase_image_href(ir, old_base_url_str, new_base_url_str, spns);
        } else if (ir->code() == code_svg_use) {
            rebase_image_href(ir, old_base_url_str, new_base_url_str, false);
        }
        return true;
    });
}

void Inkscape::XML::rebase_hrefs(SPDocument *const doc, gchar const *const new_base, bool const spns)
{
    rebase_hrefs(doc->getReprRoot(), doc->getDocumentBase(), new_base, spns);
    doc->setDocumentBase(new_base);
}


/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vi: set autoindent shiftwidth=4 tabstop=8 filetype=cpp expandtab softtabstop=4 fileencoding=utf-8 textwidth=99 :
