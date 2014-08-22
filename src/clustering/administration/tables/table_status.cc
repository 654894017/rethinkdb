// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/tables/table_status.hpp"

#include "clustering/administration/datum_adapter.hpp"
#include "clustering/administration/servers/name_client.hpp"
#include "clustering/administration/tables/lookup.hpp"

/* Names like `reactor_activity_entry_t::secondary_without_primary_t` are too long to
type without this */
using reactor_business_card_details::primary_when_safe_t;
using reactor_business_card_details::primary_t;
using reactor_business_card_details::secondary_up_to_date_t;
using reactor_business_card_details::secondary_without_primary_t;
using reactor_business_card_details::secondary_backfilling_t;
using reactor_business_card_details::nothing_when_safe_t;
using reactor_business_card_details::nothing_when_done_erasing_t;
using reactor_business_card_details::nothing_t;

static bool check_complete_set(const std::vector<reactor_activity_entry_t> &status) {
    std::vector<hash_region_t<key_range_t> > regions;
    for (const reactor_activity_entry_t &entry : status) {
        regions.push_back(entry.region);
    }
    hash_region_t<key_range_t> joined;
    region_join_result_t res = region_join(regions, &joined);
    return res == REGION_JOIN_OK &&
        joined.beg == 0 &&
        joined.end == HASH_REGION_HASH_SIZE;
}

template<class T>
static size_t count_in_state(const std::vector<reactor_activity_entry_t> &status) {
    int count = 0;
    for (const reactor_activity_entry_t &entry : status) {
        if (boost::get<T>(&entry.activity) != NULL) {
            ++count;
        }
    }
    return count;
}

counted_t<const ql::datum_t> convert_director_status_to_datum(
        const std::vector<reactor_activity_entry_t> *status) {
    ql::datum_object_builder_t object_builder;
    object_builder.overwrite("role", make_counted<const ql::datum_t>("director"));
    std::string state;
    if (!status) {
        state = "missing";
    } else if (!check_complete_set(*status)) {
        state = "transitioning";
    } else {
        size_t tally = count_in_state<primary_t>(*status);
        if (tally == status->size()) {
            state = "ready";
        } else {
            tally += count_in_state<primary_when_safe_t>(*status);
            if (tally == status->size()) {
                state = "backfilling_data";
                /* RSI(reql_admin): Implement backfill progress */
                object_builder.overwrite("backfill_progress",
                    make_counted<const ql::datum_t>("not_implemented"));
            } else {
                state = "transitioning";
            }
        }
    }
    object_builder.overwrite("state", make_counted<const ql::datum_t>(std::move(state)));
    return std::move(object_builder).to_counted();
}

counted_t<const ql::datum_t> convert_replica_status_to_datum(
        const std::vector<reactor_activity_entry_t> *status) {
    ql::datum_object_builder_t object_builder;
    object_builder.overwrite("role", make_counted<const ql::datum_t>("replica"));
    std::string state;
    if (!status) {
        state = "missing";
    } else if (!check_complete_set(*status)) {
        state = "transitioning";
    } else {
        size_t tally = count_in_state<secondary_up_to_date_t>(*status);
        if (tally == status->size()) {
            state = "ready";
        } else {
            tally += count_in_state<secondary_without_primary_t>(*status);
            if (tally == status->size()) {
                state = "looking_for_director";
            } else {
                tally += count_in_state<secondary_backfilling_t>(*status);
                if (tally == status->size()) {
                    state = "backfilling_data";
                    /* RSI(reql_admin): Implement backfill progress */
                    object_builder.overwrite("backfill_progress",
                        make_counted<const ql::datum_t>("not_implemented"));
                } else {
                    state = "transitioning";
                }
            }
        }
    }
    object_builder.overwrite("state", make_counted<const ql::datum_t>(std::move(state)));
    return std::move(object_builder).to_counted();
}

counted_t<const ql::datum_t> convert_nothing_status_to_datum(
         const std::vector<reactor_activity_entry_t> *status) {
    ql::datum_object_builder_t object_builder;
    object_builder.overwrite("role", make_counted<const ql::datum_t>("nothing"));
    std::string state;
    if (!status) {
        state = "missing";
    } else if (!check_complete_set(*status)) {
        state = "transitioning";
    } else {
        size_t tally = count_in_state<nothing_t>(*status);
        if (tally == status->size()) {
            /* This machine shouldn't even appear in the map */
            return counted_t<const ql::datum_t>();
        } else {
            tally += count_in_state<nothing_when_done_erasing_t>(*status);
            if (tally == status->size()) {
                state = "erasing_data";
            } else {
                tally += count_in_state<nothing_when_safe_t>(*status);
                if (tally == status->size()) {
                    state = "offloading_data";
                } else {
                    state = "transitioning";
                }
            }
        }
    }
    object_builder.overwrite("state", make_counted<const ql::datum_t>(std::move(state)));
    return std::move(object_builder).to_counted();
}

counted_t<const ql::datum_t> convert_table_status_shard_to_datum(
        namespace_id_t uuid,
        key_range_t range,
        const table_config_t::shard_t &shard,
        machine_id_t chosen_director,
        const change_tracking_map_t<peer_id_t, namespaces_directory_metadata_t> &dir,
        server_name_client_t *name_client) {
    /* `server_states` will contain one entry per connected server. That entry will be a
    vector with the current state of each hash-shard on the server whose key range
    matches the expected range. */
    std::map<name_string_t, std::vector<reactor_activity_entry_t> > server_states;
    for (auto it = dir.get_inner().begin(); it != dir.get_inner().end(); ++it) {

        /* Translate peer ID to machine ID */
        boost::optional<machine_id_t> machine_id =
            name_client->get_machine_id_for_peer_id(it->first);
        if (!machine_id) {
            /* This can occur as a race condition if the peer has just connected or just
            disconnected */
            continue;
        }

        /* Translate machine ID to server name */
        boost::optional<name_string_t> name =
            name_client->get_name_for_machine_id(*machine_id);
        if (!name) {
            /* This can occur if the peer was permanently removed */
            continue;
        }

        /* Extract activity from reactor business card. `server_state` may be left empty
        if the reactor doesn't have a business card for this table, or if no entry has
        the same region as the target region. */
        std::vector<reactor_activity_entry_t> server_state;
        auto jt = it->second.reactor_bcards.find(uuid);
        if (jt != it->second.reactor_bcards.end()) {
            cow_ptr_t<reactor_business_card_t> bcard = jt->second.internal;
            for (auto kt = bcard->activities.begin();
                      kt != bcard->activities.end();
                    ++kt) {
                if (kt->second.region.inner == range) {
                    server_state.push_back(kt->second);
                }
            }
        }

        server_states.insert(std::make_pair(*name, server_state));
    }

    ql::datum_object_builder_t object_builder;

    boost::optional<name_string_t> director_name =
        name_client->get_name_for_machine_id(chosen_director);
    if (director_name) {
        object_builder.overwrite(director_name->str(),
            convert_director_status_to_datum(
                server_states.count(*director_name) == 1 ?
                    &server_states[*director_name] : NULL));
    }

    for (const name_string_t &replica : shard.replica_names) {
        if (object_builder.try_get(replica.str()).has()) {
            /* Don't overwrite the director's entry */
            continue;
        }
        object_builder.overwrite(replica.str(),
            convert_replica_status_to_datum(
                server_states.count(replica) == 1 ?
                    &server_states[replica] : NULL));
    }

    std::map<name_string_t, machine_id_t> other_names =
        name_client->get_name_to_machine_id_map()->get();
    for (auto it = other_names.begin(); it != other_names.end(); ++it) {
        if (object_builder.try_get(it->first.str()).has()) {
            /* Don't overwrite a director or replica entry */
            continue;
        }
        counted_t<const ql::datum_t> entry =
            convert_nothing_status_to_datum(
                server_states.count(it->first) == 1 ?
                    &server_states[it->first] : NULL);
        if (entry.has()) {
            object_builder.overwrite(it->first.str(), entry);
        }
    }

    return std::move(object_builder).to_counted();
}

counted_t<const ql::datum_t> convert_table_status_to_datum(
        name_string_t db_name,
        name_string_t table_name,
        namespace_id_t uuid,
        const table_replication_info_t &repli_info,
        const change_tracking_map_t<peer_id_t, namespaces_directory_metadata_t> &dir,
        server_name_client_t *name_client) {
    ql::datum_object_builder_t builder;
    builder.overwrite("name", convert_db_and_table_to_datum(db_name, table_name));
    builder.overwrite("uuid", convert_uuid_to_datum(uuid));

    ql::datum_array_builder_t array_builder((ql::configured_limits_t()));
    for (size_t i = 0; i < repli_info.config.shards.size(); ++i) {
        array_builder.add(
            convert_table_status_shard_to_datum(
                uuid,
                repli_info.config.get_shard_range(i),
                repli_info.config.shards[i],
                repli_info.chosen_directors[i],
                dir,
                name_client));
    }
    builder.overwrite("shards", std::move(array_builder).to_counted());

    return std::move(builder).to_counted();
}

std::string table_status_artificial_table_backend_t::get_primary_key_name() {
    return "name";
}

bool table_status_artificial_table_backend_t::read_all_primary_keys(
        UNUSED signal_t *interruptor,
        std::vector<counted_t<const ql::datum_t> > *keys_out,
        std::string *error_out) {
    on_thread_t thread_switcher(home_thread());
    keys_out->clear();
    databases_semilattice_metadata_t databases = database_sl_view->get();
    cow_ptr_t<namespaces_semilattice_metadata_t> md = table_sl_view->get();
    for (auto it = md->namespaces.begin();
              it != md->namespaces.end();
            ++it) {
        if (it->second.is_deleted()) {
            continue;
        }
        if (it->second.get_ref().database.in_conflict() ||
                it->second.get_ref().name.in_conflict()) {
            /* TODO: Handle conflict differently */
            *error_out = "Metadata is in conflict";
            return false;
        }
        database_id_t db_id = it->second.get_ref().database.get_ref();
        auto jt = databases.databases.find(db_id);
        guarantee(jt != databases.databases.end());
        /* RSI(reql_admin): This can actually happen. We should handle this case. */
        guarantee(!jt->second.is_deleted());
        if (jt->second.get_ref().name.in_conflict()) {
            *error_out = "Metadata is in conflict";
            return false;
        }
        name_string_t db_name = jt->second.get_ref().name.get_ref();
        name_string_t table_name = it->second.get_ref().name.get_ref();
        /* TODO: How to handle table name collisions? */
        keys_out->push_back(convert_db_and_table_to_datum(db_name, table_name));
    }
    return true;
}

bool table_status_artificial_table_backend_t::read_row(
        counted_t<const ql::datum_t> primary_key,
        UNUSED signal_t *interruptor,
        counted_t<const ql::datum_t> *row_out,
        std::string *error_out) {
    on_thread_t thread_switcher(home_thread());
    cow_ptr_t<namespaces_semilattice_metadata_t> md = table_sl_view->get();
    name_string_t db_name, table_name;
    std::string dummy_error;
    namespace_id_t table_id;
    if (!convert_db_and_table_from_datum(primary_key, &db_name, &table_name,
                                               &dummy_error)) {
        /* If the primary key was not a valid table name, then it must refer to a
        nonexistent row. 
        TODO: Would it be more user-friendly to error instead? */
        table_id = nil_uuid();
    } else {
        table_id = lookup_table_with_database(db_name, table_name,
            database_sl_view->get(), md);
    }
    if (table_id == nil_uuid()) {
        *row_out = counted_t<const ql::datum_t>();
        return true;
    }

    auto it = md->namespaces.find(table_id);
    if (it->second.get_ref().replication_info.in_conflict()) {
        *error_out = "Metadata is in conflict.";
        return false;
    }
    *row_out = convert_table_status_to_datum(
        db_name,
        table_name,
        it->first,
        it->second.get_ref().replication_info.get_ref(),
        directory_view->get(),
        name_client);
    return true;
}

bool table_status_artificial_table_backend_t::write_row(
        UNUSED counted_t<const ql::datum_t> primary_key,
        UNUSED counted_t<const ql::datum_t> new_value,
        UNUSED signal_t *interruptor,
        std::string *error_out) {
    *error_out = "It's illegal to write to the `rethinkdb.table_status` table.";
    return false;
}


