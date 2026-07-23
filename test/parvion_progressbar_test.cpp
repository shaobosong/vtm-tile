// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/parvion/progressbar.hpp"
#include "netxs/apps/parvion/hash_view.hpp"
#include "netxs/apps/parvion/table.hpp"
#include "netxs/apps/parvion/transfer_view.hpp"

#include <cstdio>
#include <limits>
#include <vector>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    struct mock_canvas
    {
        struct write { rect area; cell value; };
        std::vector<write> writes;

        template<class Fx>
        void fill(rect area, Fx fx)
        {
            auto value = cell{};
            fx(value);
            writes.push_back({ area, value });
        }
    };

    auto test_fraction_clamping_and_fill_width() -> bool
    {
        return progressbar_fill_width(10, -0.5) == 0
            && progressbar_fill_width(10, 0.49) == 4
            && progressbar_fill_width(10, 1.0) == 10
            && progressbar_fill_width(10, 1.5) == 10
            && progressbar_fill_width(10, std::numeric_limits<double>::quiet_NaN()) == 0;
    }

    auto test_palette_fill_and_default_center_right_label() -> bool
    {
        auto canvas = mock_canvas{};
        auto track = ui32{ 0xFF010203u };
        auto fill = ui32{ 0xFF040506u };
        auto fg = ui32{ 0xFF070809u };
        auto value = progressbar_value{ .fraction = 0.5,
                                        .label = "50%",
                                        .palette = { track, fill, fg } };
        progressbar_render(value, canvas, rect{{ 2, 3 }, { 8, 1 }});
        if (canvas.writes.size() != 5) return faux; // Track + fill + three label glyphs.
        return canvas.writes[0].area == rect{{ 2, 3 }, { 8, 1 }}
            && canvas.writes[0].value.bgc() == argb{ track }
            && canvas.writes[1].area == rect{{ 2, 3 }, { 4, 1 }}
            && canvas.writes[1].value.bgc() == argb{ fill }
            && canvas.writes[2].area.coor == twod{ 6, 3 }
            && canvas.writes[2].value.fgc() == argb{ fg };
    }

    auto test_percentage_alignment_preserves_centered_field_edges() -> bool
    {
        auto area = rect{{ 2, 3 }, { 11, 1 }};
        auto zero_width = cell_width("0.00%");
        auto partial_width = cell_width("12.34%");
        auto full_width = cell_width("100.00%");
        auto center_left_zero = progressbar_label_x(area, zero_width, progressbar_alignment::center_left);
        auto center_left_full = progressbar_label_x(area, full_width, progressbar_alignment::center_left);
        auto center_right_zero = progressbar_label_x(area, zero_width, progressbar_alignment::center_right);
        auto center_right_partial = progressbar_label_x(area, partial_width, progressbar_alignment::center_right);
        auto center_right_full = progressbar_label_x(area, full_width, progressbar_alignment::center_right);
        auto odd_area = rect{{ 0, 0 }, { 10, 1 }};
        return center_left_zero == center_left_full
            && center_left_zero == 4
            && center_right_zero + zero_width == center_right_partial + partial_width
            && center_right_zero + zero_width == center_right_full + full_width
            && center_right_full + full_width == 11
            && progressbar_label_x(odd_area, full_width, progressbar_alignment::center_right) == 1
            && progressbar_label_x(area, full_width, progressbar_alignment::left) == 2
            && progressbar_label_x(area, zero_width, progressbar_alignment::right) + zero_width == 13;
    }

    auto test_embedded_clip_stays_inside_viewport() -> bool
    {
        auto canvas = mock_canvas{};
        auto value = progressbar_value{ .fraction = 0.5, .label = "50%" };
        progressbar_render(value, canvas, rect{{ -3, 2 }, { 8, 1 }}, rect{{ 0, 2 }, { 5, 1 }});
        if (canvas.writes.empty()) return faux;
        for (auto& write : canvas.writes)
        {
            if (write.area.coor.x < 0 || write.area.coor.x + write.area.size.x > 5) return faux;
            if (write.area.coor.y != 2 || write.area.size.y != 1) return faux;
        }
        return true;
    }

    auto test_transfer_cell_uses_progressbar_contract() -> bool
    {
        auto cache = xfer_progress_cache{};
        auto first = xfer_progress_cell(cache, 7, 0.375, "37.50%", theme::err_fg);
        auto first_component = std::get_if<table_component_cell>(&first.value);
        if (!first_component || !first_component->content.widget) return faux;
        auto first_widget = first_component->content.widget;
        auto second = xfer_progress_cell(cache, 7, 0.5, "50.00%", theme::dir_fg);
        auto second_component = std::get_if<table_component_cell>(&second.value);
        auto& state = *cache.entries.at(7).state;
        return second_component
            && second_component->content.widget == first_widget
            && state.fraction == 0.5
            && state.label == "50.00%"
            && state.foreground == theme::dir_fg;
    }

    auto test_checksum_cell_uses_stable_progressbar() -> bool
    {
        auto cache = progressbar_cache{};
        auto item = hash_item{};
        item.id = 42;
        item.status = hash_item::hashing;
        item.size = 200;
        item.done = 90;

        auto first = hash_progress_cell(cache, item);
        auto first_component = std::get_if<table_component_cell>(&first.value);
        if (!first_component || !first_component->content.widget) return faux;
        auto first_widget = first_component->content.widget;
        auto& first_state = *cache.entries.at(item.id).state;
        if (first_state.fraction != 0.45 || first_state.label != "45.00%"
         || first_state.foreground != theme::text_fg) return faux;

        item.done = item.size;
        item.status = hash_item::succeeded;
        auto second = hash_progress_cell(cache, item);
        auto second_component = std::get_if<table_component_cell>(&second.value);
        auto& second_state = *cache.entries.at(item.id).state;
        return second_component
            && second_component->content.widget == first_widget
            && second_state.fraction == 1.0
            && second_state.label == "100.00%"
            && second_state.foreground == theme::dir_fg;
    }

    auto test_server_caption_and_hash_size_width() -> bool
    {
        return server_source("files.example", "alice", 22) == "alice@files.example:22"
            && server_source("files.example", {}, 2222) == "files.example:2222"
            && q_headers[(size_t)q_server] == "Server"
            && q_headers[(size_t)q_local] == "Local Name"
            && q_col_fit_w(hash_headers[3], hash_size_body_w, true) == 9
            && xfer_reason_w(xfer_cols{}, 1) == 31;
    }
}

int main()
{
    auto ok = test_fraction_clamping_and_fill_width()
           && test_palette_fill_and_default_center_right_label()
           && test_percentage_alignment_preserves_centered_field_edges()
           && test_embedded_clip_stays_inside_viewport()
           && test_transfer_cell_uses_progressbar_contract()
           && test_checksum_cell_uses_stable_progressbar()
           && test_server_caption_and_hash_size_width();
    if (!ok) std::fprintf(stderr, "parvion progressbar tests failed\n");
    return ok ? 0 : 1;
}
