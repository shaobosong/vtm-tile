// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/label.hpp: A retained text label with semantic text and
// hint roles.  Overflow handling and measurement use terminal display cells
// so layout agrees with the renderer for wide and combining graphemes.

#include "ui.hpp"

namespace netxs::app::parvion
{
    enum class label_role
    {
        text,
        hint,
    };

    enum class label_overflow
    {
        clip,
        ellipsis,
        wrap,
    };

    struct label_palette
    {
        ui32 text = theme::text_fg;
        ui32 hint = theme::subtext;
        ui32 background = theme::bg;
    };

    struct label_cfg
    {
        std::function<text()> value;
        label_role role = label_role::text;
        label_overflow overflow = label_overflow::clip;
        label_palette palette{};
    };

    inline auto label_explicit_lines(view source) -> std::vector<text>
    {
        auto result = std::vector<text>{};
        auto start = size_t{};
        for (;;)
        {
            auto stop = source.find('\n', start);
            result.emplace_back(source.substr(start, stop == view::npos ? view::npos : stop - start));
            if (stop == view::npos) break;
            start = stop + 1;
        }
        return result;
    }

    inline auto label_split_word(view word, si32 width) -> std::vector<text>
    {
        auto result = std::vector<text>{};
        auto line = text{};
        auto used = si32{};
        utf::decode_clusters(word, [&](view cluster)
        {
            auto cells = gc_cells(utf::cluster(cluster));
            if (!line.empty() && used + cells > width)
            {
                result.push_back(std::move(line));
                line.clear();
                used = 0;
            }
            line += text{ cluster };
            used += cells;
            return true;
        });
        if (!line.empty()) result.push_back(std::move(line));
        return result;
    }

    inline auto wrap_label_text(view source, si32 width) -> std::vector<text>
    {
        if (width <= 0) return label_explicit_lines(source);
        auto result = std::vector<text>{};
        for (auto& paragraph : label_explicit_lines(source))
        {
            if (paragraph.empty())
            {
                result.emplace_back();
                continue;
            }
            auto line = text{};
            auto cursor = size_t{};
            while (cursor < paragraph.size())
            {
                while (cursor < paragraph.size() && paragraph[cursor] == ' ') ++cursor;
                if (cursor == paragraph.size()) break;
                auto stop = paragraph.find(' ', cursor);
                if (stop == text::npos) stop = paragraph.size();
                auto word = view{ paragraph }.substr(cursor, stop - cursor);
                cursor = stop;

                auto candidate = line.empty() ? text{ word } : line + " " + text{ word };
                if (cell_width(candidate) <= width)
                {
                    line = std::move(candidate);
                    continue;
                }
                if (!line.empty())
                {
                    result.push_back(std::move(line));
                    line.clear();
                }
                if (cell_width(word) <= width)
                {
                    line = text{ word };
                }
                else
                {
                    auto pieces = label_split_word(word, width);
                    for (auto i = size_t{}; i + 1 < pieces.size(); ++i)
                        result.push_back(std::move(pieces[i]));
                    if (!pieces.empty()) line = std::move(pieces.back());
                }
            }
            if (!line.empty()) result.push_back(std::move(line));
        }
        if (result.empty()) result.emplace_back();
        return result;
    }

    class label
        : public ui::form<label>
    {
        label_cfg config;
        std::vector<text> lines;

        auto current() const -> text
        {
            return config.value ? config.value() : text{};
        }

        auto foreground() const -> ui32
        {
            return config.role == label_role::hint ? config.palette.hint
                                                   : config.palette.text;
        }

        auto layout(view value, si32 width) const -> std::vector<text>
        {
            if (config.overflow == label_overflow::wrap)
                return wrap_label_text(value, width);

            auto result = label_explicit_lines(value);
            if (config.overflow == label_overflow::ellipsis)
                for (auto& line : result) line = fit_ellipsis(line, width);
            return result;
        }

        auto tooltip() const -> text
        {
            if (config.overflow != label_overflow::ellipsis) return {};
            auto value = current();
            auto width = base::size().x;
            for (auto& line : label_explicit_lines(value))
                if (cell_width(line) > width) return value;
            return {};
        }

    protected:
        void deform(rect& new_area) override
        {
            auto value = current();
            auto width = new_area.size.x;
            if (width <= 0)
            {
                auto explicit_lines = label_explicit_lines(value);
                for (auto& line : explicit_lines) width = std::max(width, cell_width(line));
                new_area.size.x = width;
            }
            lines = layout(value, width);
            new_area.size.y = std::max(new_area.size.y, std::max(1, (si32)lines.size()));
        }

    public:
        static constexpr auto classname = basename::label;

        label(label_cfg setup)
            : config{ std::move(setup) }
        {
            on(tier::mouserelease, input::key::MouseHover, [&](hids&)
            {
                base::signal(tier::preview, e2::form::prop::ui::tooltip, tooltip());
            });
            LISTEN(tier::release, e2::render::any, canvas)
            {
                auto size = base::size();
                canvas.fill(rect{ {}, size }, [&](cell& c)
                {
                    c.bgc(config.palette.background).fgc(foreground()).txt(whitespace);
                });
                auto render_lines = layout(current(), size.x);
                for (auto y = si32{}; y < size.y && y < (si32)render_lines.size(); ++y)
                    put_str(canvas, 0, y, render_lines[(size_t)y], foreground(), config.palette.background, size.x);
            };
        }

        auto get_lines() const -> std::vector<text> const& { return lines; }
        auto get_foreground() const -> ui32 { return foreground(); }
        auto get_tooltip() const -> text { return tooltip(); }

        static auto ctor(label_cfg setup)
        {
            auto has_tooltip = setup.overflow == label_overflow::ellipsis;
            auto item = ui::tui_domain().create<label>(std::move(setup));
            if (has_tooltip) item->active()->template plugin<pro::notes>();
            return item;
        }
    };

    inline auto make_label(label_cfg cfg) -> component
    {
        return { label::ctor(std::move(cfg)) };
    }
}
