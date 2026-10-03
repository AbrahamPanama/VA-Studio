// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Author:
 *   Gustav Broberg <broberg@kth.se>
 *   Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (c) 2014 Authors
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#include "event-log.h"

#include <glibmm/i18n.h>
#include <vector>

#include "document-undo.h"
#include "document.h"
#include "actions/actions-undo-document.h"

namespace Inkscape {

EventLog::EventModelColumns const &EventLog::getColumns()
{
    static const EventModelColumns columns;
    return columns;
}

EventLog::EventLog(SPDocument *document)
    : _document{document}
    , _event_list_store{Gtk::TreeStore::create(getColumns())}
{
    // add initial pseudo event
    Gtk::TreeRow curr_row = *_event_list_store->append();
    _curr_event = _first_event = _last_event = curr_row.get_iter();
    
    auto &_columns = getColumns();
    curr_row[_columns.description] = _("[No more changes]");
    curr_row[_columns.icon_name] = "document-new";
    curr_row[_columns.child_count] = 0;
    curr_row[_columns.serial] = _next_serial++;

    // The initial pseudo row is the document's starting state and, with no
    // edits committed yet, the state on disk. Anchor it so undoing back to the
    // start is clean again instead of prompting to save an unmodified document.
    _last_saved_serial = curr_row[_columns.serial];
}

void EventLog::rememberFileSave(std::uint64_t serial)
{
    _last_saved_serial = serial;
    // A real save published the current state; the pending action key must not
    // let the next keyed edit coalesce into the saved Undo row, or a later
    // Undo/Redo would present that merged row as the clean state while disk
    // holds only the pre-merge bytes.
    if (_document) {
        _document->resetKey();
    }
}

EventLog::iterator EventLog::findEventBySerial(std::uint64_t serial) const
{
    if (!serial) return iterator{nullptr};
    if ((*_curr_event)[getColumns().serial] == serial) return _curr_event;
    for (auto parent = _event_list_store->children().begin(); parent != _event_list_store->children().end(); ++parent) {
        if ((*parent)[getColumns().serial] == serial) return parent;
        for (auto child = parent->children().begin(); child != parent->children().end(); ++child) {
            if ((*child)[getColumns().serial] == serial) return child;
        }
    }
    return iterator{nullptr};
}

EventLog::~EventLog() = default;

void EventLog::notifyUndoEvent(Event *log)
{
    if (_blocker.pending()) {
        return;
    }

    auto &_columns = getColumns();

    // make sure the supplied event matches the next undoable event
    g_return_if_fail(_getUndoEvent() && (*(_getUndoEvent()))[_columns.event] == log);

    // if we're on the first child event...
    if (_curr_event->parent() && _curr_event == _curr_event->parent()->children().begin()) {
        // ...back up to the parent
        _curr_event = _curr_event->parent();
        _curr_event_parent = (iterator)nullptr;
    } else {
        --_curr_event;

        // if we're entering a branch, move to the end of it
        if (!_curr_event->children().empty()) {
            _curr_event_parent = _curr_event;
            _curr_event = _curr_event->children().end();
            --_curr_event;
        }
    }

    _checkForVirginity();
    updateUndoVerbs();

    _row_changed.emit();
}

void EventLog::notifyRedoEvent(Event *log)
{
    if (_blocker.pending()) {
        return;
    }

    auto &_columns = getColumns();

    // make sure the supplied event matches the next redoable event
    g_return_if_fail(_getRedoEvent() && (*(_getRedoEvent()))[_columns.event] == log);

    // if we're on a parent event...
    if (!_curr_event->children().empty()) {
        // ...move to its first child
        _curr_event_parent = _curr_event;
        _curr_event = _curr_event->children().begin();
    } else {
        ++_curr_event;

        // if we are about to leave a branch...
        if (_curr_event->parent() && _curr_event == _curr_event->parent()->children().end()) {
            // ...and move to the next event at parent level
            _curr_event = _curr_event->parent();
            _curr_event_parent = (iterator)nullptr;
            ++_curr_event;
        }
    }

    _checkForVirginity();
    updateUndoVerbs();

    _row_changed.emit();
}

void EventLog::notifyUndoCommitEvent(Event *log)
{
    auto icon_name = log->icon_name;

    Gtk::TreeRow curr_row;
    auto &_columns = getColumns();

    // if the new event is of the same type as the previous then create a new branch
    if (icon_name == Glib::ustring{(*_curr_event)[_columns.icon_name]}) {
        if (!_curr_event_parent) {
            _curr_event_parent = _curr_event;
        }
        curr_row = *_event_list_store->append(_curr_event_parent->children());
        (*_curr_event_parent)[_columns.child_count] = _curr_event_parent->children().size() + 1;
    } else {
        curr_row = *_event_list_store->append();
        curr_row[_columns.child_count] = 1;
        _curr_event_parent = iterator{nullptr};
    }

    _curr_event = _last_event = curr_row.get_iter();

    curr_row[_columns.event] = log;
    curr_row[_columns.icon_name] = icon_name;
    curr_row[_columns.description] = log->description;
    curr_row[_columns.serial] = _next_serial++;

    _checkForVirginity();
    updateUndoVerbs();

    _row_changed.emit();
}

void EventLog::notifyUndoExpired(Event *log)
{
    auto &columns = getColumns();

    if (_event_list_store->children().size() == 1) {
        return; // Nothing to do, nothing in the undo log
    }

    // We only have to look at one item because we never expire from the middle.
    iterator iter = _event_list_store->children().begin();

    // Skip first item, it's the non-event label
    if (iter == _first_event) {
        iter++;
    }

    assert((*iter)[columns.event] == log);

    iterator to_remove;
    if (iter->children().size() > 0) {
        // Move first child's log to parent as the parent is being deleted
        to_remove = iter->children().begin();

        // The parent is repurposed for the child's state. Its old serial must
        // not name the new state, and the removed child's serial disappears.
        (*iter)[columns.serial] = _next_serial++;

        Event *child_log = (*to_remove)[columns.event];
        Glib::ustring desc = (*to_remove)[columns.description];
        (*iter)[columns.event] = child_log;
        (*iter)[columns.description] = desc;
    } else {
        to_remove = iter;
    }

    // This should never happen as we should never expire undo items from the middle.
    assert(to_remove->children().size() == 0);

    if (auto parent = to_remove->parent()) {
        (*parent)[columns.child_count] = to_remove->parent()->children().size() - 1;
    }

    if (_curr_event == to_remove) {
        _curr_event = to_remove == iter ? _first_event : iter;
        _curr_event_parent = iterator{nullptr};
    }
    if (_last_event == to_remove) _last_event = to_remove == iter ? _first_event : iter;

    _event_list_store->erase(to_remove);

    // Expiry advances the oldest reachable state even when the pseudo row is
    // unchanged as a tree node. It must no longer match a saved old state.
    (*_first_event)[columns.serial] = _next_serial++;
    if (_last_saved_serial && !findEventBySerial(_last_saved_serial)) invalidateFileSave();

    // Tell the user about the forgotten undo stack
    if ((*_first_event)[columns.child_count] == 0) {
        (*_first_event)[columns.description] = _("[Changes forgotten]");
    }
    (*_first_event)[columns.child_count] = (*_first_event)[columns.child_count] + 1;
}

void EventLog::notifyClearUndoEvent()
{
    // The document is about to drop every Undo step, so the rows up to and
    // including the current one must go too; otherwise the Undo History keeps
    // rows that no longer exist and seekTo() walks a stale model. Rows after the
    // current one are Redo steps, which DocumentUndo::clearUndo() keeps.
    auto guard = _blocker.block();
    auto &columns = getColumns();

    struct RedoRow
    {
        Event *event;
        Glib::ustring icon_name;
        Glib::ustring description;
        std::uint64_t serial;
    };
    std::vector<RedoRow> redo;
    bool past_current = (_curr_event == _first_event);
    auto visit = [&](iterator it) {
        if (past_current && it != _first_event) {
            Event *event = (*it)[columns.event];
            redo.push_back({event, (*it)[columns.icon_name], (*it)[columns.description],
                            (*it)[columns.serial]});
        }
        if (it == _curr_event) past_current = true;
    };
    for (auto parent = _event_list_store->children().begin(); parent != _event_list_store->children().end(); ++parent) {
        visit(parent);
        for (auto child = parent->children().begin(); child != parent->children().end(); ++child) {
            visit(child);
        }
    }

    // Erase every root row after the start row (children go with their parent).
    auto first = _first_event;
    ++first;
    while (first != _event_list_store->children().end()) {
        first = _event_list_store->erase(first);
    }

    // Surviving Redo steps become flat rows after the start row.
    _last_event = _first_event;
    for (auto const &row : redo) {
        auto new_row = *_event_list_store->append();
        new_row[columns.event] = row.event;
        new_row[columns.icon_name] = row.icon_name;
        new_row[columns.description] = row.description;
        new_row[columns.child_count] = 1;
        new_row[columns.serial] = row.serial;
        _last_event = new_row.get_iter();
    }

    // The start row now names the current document state; give it a serial no
    // old row or saved anchor can match.
    bool const clean = _document && !_document->isModifiedSinceSave();
    _curr_event = _first_event;
    _curr_event_parent = iterator{nullptr};
    (*_first_event)[columns.serial] = _next_serial++;
    if (clean) {
        _last_saved_serial = (*_first_event)[columns.serial];
    } else if (_last_saved_serial && !findEventBySerial(_last_saved_serial)) {
        // The saved state is no longer reachable by Undo/Redo, and the
        // document holds changes: no position may report clean.
        invalidateFileSave();
    }

    updateUndoVerbs();
    _row_changed.emit();
}

void EventLog::notifyClearRedoEvent()
{
    _clearRedo();
    updateUndoVerbs();
}

// Enable/disable undo/redo GUI items.
void EventLog::updateUndoVerbs()
{
    if (_document) {
        enable_undo_actions(_document, static_cast<bool>(_getUndoEvent()), static_cast<bool>(_getRedoEvent()));
    }
}

void EventLog::seekTo(iterator target)
{
    if (_blocker.pending()) {
        return;
    }
    auto guard = _blocker.block();

    assert(target);

    if (_event_list_store->get_path(target) < _event_list_store->get_path(_curr_event)) {

        // An event before the current one has been selected. Undo to the selected event.
        while (_curr_event != target) {
            // Stop and keep the rows in step with the document when the
            // document could not undo (stack cleared or history locked).
            if (!DocumentUndo::undo(_document)) break;

            if (_curr_event->parent() && _curr_event == _curr_event->parent()->children().begin()) {
                _curr_event = _curr_event->parent();
                _curr_event_parent = {};
            } else {
                --_curr_event;
                if (!_curr_event->children().empty()) {
                    _curr_event_parent = _curr_event;
                    _curr_event = _curr_event->children().end();
                    --_curr_event;
                }
            }
        }

    } else {

        // An event after the current one has been selected. Redo to the selected event.
        while (target != _curr_event) {
            if (!DocumentUndo::redo(_document)) break;

            if (!_curr_event->children().empty()) {
                _curr_event_parent = _curr_event;
                _curr_event = _curr_event->children().begin();
            } else {
                ++_curr_event;
                if (_curr_event->parent() && _curr_event == _curr_event->parent()->children().end()) {
                    _curr_event = _curr_event->parent();
                    ++_curr_event;
                    _curr_event_parent = {};
                }
            }
        }
    }

    _checkForVirginity();
    updateUndoVerbs();

    _row_changed.emit();
}

EventLog::const_iterator EventLog::_getUndoEvent() const
{
    if (_curr_event == _event_list_store->children().begin()) {
        return const_iterator{nullptr};
    }
    return _curr_event;
}

EventLog::const_iterator EventLog::_getRedoEvent() const
{
    if (_curr_event == _last_event) {
        return const_iterator{nullptr};
    }

    if (!_curr_event->children().empty()) {
        return _curr_event->children().begin();
    }

    auto redo_event = _curr_event;
    ++redo_event;

    if (redo_event->parent() && redo_event == redo_event->parent()->children().end()) {
        redo_event = redo_event->parent();
        ++redo_event;
    }

    return redo_event;
}

void EventLog::_clearRedo()
{
    auto guard = _blocker.block();

    if (_last_event == _curr_event) {
        return;
    }

    auto const &columns = getColumns();
    auto erase_siblings = [&](iterator first, iterator end) {
        while (first != end) {
            first = _event_list_store->erase(first);
        }
    };

    // Prune the current group's redo children before walking root rows. Child
    // and root end iterators belong to different models in gtkmm.
    if (!_curr_event->children().empty()) {
        erase_siblings(_curr_event->children().begin(), _curr_event->children().end());
        (*_curr_event)[columns.child_count] = 1;
    } else if (auto parent = _curr_event->parent()) {
        auto next_child = _curr_event;
        ++next_child;
        erase_siblings(next_child, parent->children().end());
        (*parent)[columns.child_count] = parent->children().size() + 1;
    }

    auto next_root = _curr_event->parent() ? _curr_event->parent() : _curr_event;
    ++next_root;
    erase_siblings(next_root, _event_list_store->children().end());
    _last_event = _curr_event;
    if (_last_saved_serial && !findEventBySerial(_last_saved_serial)) invalidateFileSave();
}

// Mark document as untouched if we reach a state where the document was previously saved.
void EventLog::_checkForVirginity()
{
    g_return_if_fail(_document);
    // A missing serial means no Undo position corresponds to disk. Erased or
    // repurposed rows cannot match their old serial even if tree nodes are reused.
    if (_last_saved_serial && (*_curr_event)[getColumns().serial] == _last_saved_serial) {
        _document->setModifiedSinceSave(false);
    }
}

} // namespace Inkscape

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
