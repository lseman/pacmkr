//! pacmkr GUI — modern GTK4 package manager dashboard.
#include <gtkmm.h>
#include <glibmm/main.h>
#include <cairomm/pattern.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pacmkr/gui/backend.h"
#include "pacmkr/backend/aur_cache.h"
#include "pacmkr/backend/alpm.h"

namespace pacmkr::gui {
namespace {

// ── Helpers ────────────────────────────────────────────────────────

Gtk::Label* label(std::string const& text, std::vector<Glib::ustring> classes = {}) {
    auto* r = Gtk::make_managed<Gtk::Label>(text);
    r->set_xalign(0);
    if (!classes.empty()) r->set_css_classes(classes);
    return r;
}

Gtk::Box* empty_state(std::string const& title, std::string const& detail) {
    auto* b = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 9);
    b->set_css_classes({"empty"}); b->set_valign(Gtk::Align::CENTER);
    auto* h = label(title, {"empty-title"}); h->set_halign(Gtk::Align::CENTER);
    auto* d = label(detail, {"dim"}); d->set_halign(Gtk::Align::CENTER); d->set_wrap(true);
    b->append(*h); b->append(*d); return b;
}

void clear(Gtk::Box& box) {
    while (auto* c = box.get_first_child()) box.remove(*c);
}

// ── Theme watcher (XDG portal color-scheme) ────────────────────────

class ThemeWatcher final {
public:
    explicit ThemeWatcher(std::function<void(bool)> on_change) : on_change_(std::move(on_change)) {
        connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
        if (!connection_) return;
        subscription_id_ = g_dbus_connection_signal_subscribe(
            connection_, "org.freedesktop.portal.Desktop",
            "org.freedesktop.portal.Settings", "SettingChanged",
            "/org/freedesktop/portal/desktop", nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, &ThemeWatcher::setting_changed, this, nullptr);
        if (!subscription_id_) return;
        g_dbus_connection_call(connection_, "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Settings", "Read",
            g_variant_new("(ss)", "org.freedesktop.appearance", "color-scheme"),
            G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &ThemeWatcher::read_ready, this);
    }
    ~ThemeWatcher() {
        if (connection_ && subscription_id_) g_dbus_connection_signal_unsubscribe(connection_, subscription_id_);
        if (connection_) g_object_unref(connection_);
    }
    ThemeWatcher(ThemeWatcher const&) = delete;
    ThemeWatcher& operator=(ThemeWatcher const&) = delete;

private:
    static void apply(ThemeWatcher* self, GVariant* value) {
        if (!value) return;
        GVariant* inner = g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT) ? g_variant_get_variant(value) : g_variant_ref(value);
        if (g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32) && self->on_change_)
            self->on_change_(g_variant_get_uint32(inner) == 1);
        g_variant_unref(inner);
    }
    static void read_ready(GObject*, GAsyncResult* result, gpointer data) {
        auto* self = static_cast<ThemeWatcher*>(data);
        GVariant* reply = g_dbus_connection_call_finish(self->connection_, result, nullptr);
        if (!reply) return;
        GVariant* value = nullptr;
        g_variant_get(reply, "(v)", &value);
        apply(self, value);
        if (value) g_variant_unref(value);
        g_variant_unref(reply);
    }
    static void setting_changed(GDBusConnection*, gchar const*, gchar const*, gchar const*,
                                gchar const*, GVariant* params, gpointer data) {
        auto* self = static_cast<ThemeWatcher*>(data);
        gchar const* ns = nullptr, *key = nullptr; GVariant* value = nullptr;
        g_variant_get(params, "(&s&sv)", &ns, &key, &value);
        if (ns && key && g_str_equal(ns, "org.freedesktop.appearance") && g_str_equal(key, "color-scheme"))
            apply(self, value);
        if (value) g_variant_unref(value);
    }
    std::function<void(bool)> on_change_;
    GDBusConnection* connection_ = nullptr;
    guint subscription_id_ = 0;
};

// ── Activity / progress graph widget ───────────────────────────────

class ProgressGraph final : public Gtk::DrawingArea {
public:
    ProgressGraph() {
        set_content_height(100); set_hexpand(true);
        set_draw_func(sigc::mem_fun(*this, &ProgressGraph::draw));
    }
    void record(double value) {
        values_.pop_front(); values_.push_back(value); queue_draw();
    }
private:
    void draw(Cairo::RefPtr<Cairo::Context> const& cr, int width, int height) {
        constexpr double pad = 10.0;
        cr->set_line_width(1); cr->set_source_rgba(.55,.65,.59,.14);
        for (int line = 1; line < 4; ++line) { auto y = height * line / 4.0; cr->move_to(pad, y); cr->line_to(width - pad, y); } cr->stroke();

        if (values_.empty()) return;
        double maximum = 1.0;
        for (double v : values_) maximum = std::max(maximum, v);

        auto x_at = [&](size_t i) { return pad + i * (width - 2*pad) / (values_.size() - 1.0); };
        auto y_at = [&](double val) { return height - pad - val * (height - 2*pad) / maximum; };

        auto fill = Cairo::LinearGradient::create(0, y_at(maximum), 0, height - pad);
        fill->add_color_stop_rgba(0, .49, .42, .94, .30);
        fill->add_color_stop_rgba(1, .49, .42, .94, .02);
        cr->move_to(x_at(0), height - pad);
        size_t i = 0; for (double v : values_) { cr->line_to(x_at(i), y_at(v)); ++i; }
        cr->line_to(x_at(values_.size()-1), height - pad); cr->close_path();
        cr->set_source(fill); cr->fill();

        cr->set_source_rgba(.49, .42, .94, .95); cr->set_line_width(2.2);
        i = 0; for (double v : values_) { auto x = x_at(i), y = y_at(v); if (!i) cr->move_to(x, y); else cr->line_to(x, y); ++i; }
        cr->stroke();
    }
    std::deque<double> values_;
};

// ── MainWindow ─────────────────────────────────────────────────────

class MainWindow final : public Gtk::ApplicationWindow {
public:
    using NotificationSink = std::function<void(std::string const& msg)>;

    explicit MainWindow(Glib::RefPtr<Gtk::Application> const& app, NotificationSink notify = {})
        : Gtk::ApplicationWindow(app), notify_(std::move(notify)) {
        set_title("pacmkr"); set_default_size(1180, 760); set_size_request(900, 600);
        set_css_classes({"pacmkr"});
        signal_close_request().connect([this] {
            if (auto application = get_application()) application->quit();
            return true;
        }, false);

        appearance_ = load_appearance_preference();
        check_interval_hours_ = load_check_interval();
        last_check_ = load_last_check();
        build_ui();
        apply_appearance(appearance_);
        start_schedule_timer();
        Glib::signal_idle().connect_once([this] { refresh_overview(); check_updates_if_due(); });
    }

private:
    // ── Appearance ─────────────────────────────────────────────────

    static std::filesystem::path prefs_path() {
        return std::filesystem::path(Glib::get_user_config_dir()) / "pacmkr" / "gui.conf";
    }
    static std::string load_appearance_preference() {
        auto kf = Glib::KeyFile::create();
        try { if (kf->load_from_file(prefs_path().string())) return kf->get_string("appearance", "theme"); } catch (Glib::Error const&) {}
        return "system";
    }
    static void save_appearance_preference(std::string const& theme) {
        auto path = prefs_path(); std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec);
        auto kf = Glib::KeyFile::create();
        try { kf->load_from_file(path.string()); } catch (Glib::Error const&) {}
        kf->set_string("appearance", "theme", theme);
        try { kf->save_to_file(path.string()); } catch (Glib::Error const&) {}
    }

    static int load_check_interval() {
        auto kf = Glib::KeyFile::create();
        try {
            if (kf->load_from_file(prefs_path().string()))
                return std::max(0, kf->get_integer("updates", "interval_hours"));
        } catch (Glib::Error const&) {}
        return 24;
    }

    static std::time_t load_last_check() {
        auto kf = Glib::KeyFile::create();
        try {
            if (kf->load_from_file(prefs_path().string()))
                return static_cast<std::time_t>(kf->get_int64("updates", "last_check"));
        } catch (Glib::Error const&) {}
        return 0;
    }

    static void save_update_preferences(int interval, std::time_t last_check) {
        auto path = prefs_path(); std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec);
        auto kf = Glib::KeyFile::create();
        try { kf->load_from_file(path.string()); } catch (Glib::Error const&) {}
        kf->set_integer("updates", "interval_hours", interval);
        kf->set_int64("updates", "last_check", static_cast<gint64>(last_check));
        try { kf->save_to_file(path.string()); } catch (Glib::Error const&) {}
    }

    void apply_appearance(std::string const& preference) {
        // Manual find since we're C++17, not C++20
        std::string found = "system";
        for (auto const* p : {"system", "light", "dark"}) {
            if (p == preference) { found = std::string(p); break; }
        }
        appearance_ = found;
        save_appearance_preference(appearance_); theme_watcher_.reset();
        if (appearance_ == "system")
            theme_watcher_ = std::make_unique<ThemeWatcher>([this](bool dark) {
                Glib::signal_idle().connect_once([this, dark] { set_palette(dark ? "dark" : "light"); });
            });
        else set_palette(appearance_);
    }

    void set_palette(std::string_view p) {
        remove_css_class("dark");
        if (p == "dark") add_css_class("dark");
    }

    // ── UI Builders ────────────────────────────────────────────────

    Gtk::Box* page(std::string const& title, std::string const& subtitle, Gtk::Box*& body, Gtk::Widget* tool = nullptr) {
        auto* r = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 20);
        r->set_css_classes({"content"}); r->set_hexpand(true); r->set_vexpand(true);
        auto* top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
        auto* titles = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 3); titles->set_hexpand(true);
        titles->append(*label(title, {"page-title"})); titles->append(*label(subtitle, {"subtitle"}));
        top->append(*titles);
        if (tool) top->append(*tool);
        r->append(*top);
        auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>(); scroll->set_vexpand(true);
        body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10); scroll->set_child(*body);
        r->append(*scroll); return r;
    }

    Gtk::ToggleButton* nav(std::string const& icon_name, std::string const& text, std::string const& target) {
        auto* btn = Gtk::make_managed<Gtk::ToggleButton>();
        btn->set_css_classes({"flat", "nav"}); btn->set_hexpand(true);
        btn->set_group(*nav_group_);
        auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* icon = Gtk::make_managed<Gtk::Image>(); icon->set_from_icon_name(icon_name); icon->set_pixel_size(16);
        content->append(*icon); content->append(*label(text)); btn->set_child(*content);
        btn->set_tooltip_text(text);
        btn->signal_toggled().connect([this, target, btn]() { if (btn->get_active()) stack_->set_visible_child(target); });
        nav_buttons_[target] = btn; return btn;
    }

    void navigate(std::string const& target) {
        if (auto found = nav_buttons_.find(target); found != nav_buttons_.end())
            found->second->set_active(true);
        else stack_->set_visible_child(target);
    }

    Gtk::Box* stat_card(std::string const& name, Gtk::Label*& value) {
        auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        card->set_css_classes({"card", "stat-card"}); card->set_hexpand(true);
        card->append(*label(name, {"stat-name"}));
        value = label("0", {"stat-value"}); card->append(*value);
        return card;
    }

    // ── Build UI ───────────────────────────────────────────────────

    void build_ui() {
        auto css_provider = Gtk::CssProvider::create();
        try {
            css_provider->load_from_resource("/com/pacmkr/dashboard/pacmkr.css");
            Gtk::StyleContext::add_provider_for_display(
                Gdk::Display::get_default(), css_provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        } catch (Glib::Error const&) {}

        // Header bar
        auto* header = Gtk::make_managed<Gtk::HeaderBar>();
        header->set_css_classes({"gui-header"}); header->set_show_title_buttons(true);
        auto* brand = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 9);
        auto* brand_icon = Gtk::make_managed<Gtk::Image>();
        brand_icon->set_from_icon_name("package-installed-symbolic");
        brand_icon->set_pixel_size(20); brand_icon->set_css_classes({"brand-icon"});
        brand->append(*brand_icon); brand->append(*label("pacmkr", {"brand"}));
        header->set_title_widget(*brand); set_titlebar(*header);

        // Sidebar
        auto* layout = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        auto* sidebar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        sidebar->set_css_classes({"sidebar"}); sidebar->set_hexpand(false);

        auto* side_brand = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 7);
        side_brand->set_margin_start(5); side_brand->set_margin_top(3); side_brand->set_margin_bottom(7);
        auto* side_icon = Gtk::make_managed<Gtk::Image>();
        side_icon->set_from_icon_name("package-installed-symbolic"); side_icon->set_pixel_size(18);
        side_brand->append(*side_icon); side_brand->append(*label("pacmkr", {"sidebar-brand"}));
        sidebar->append(*side_brand);

        sidebar->append(*label("PACKAGE MANAGER", {"sidebar-section"}));
        nav_group_ = Gtk::make_managed<Gtk::ToggleButton>();
        sidebar->append(*nav("view-grid-symbolic", "Overview", "overview"));
        sidebar->append(*nav("system-search-symbolic", "Search", "search"));
        sidebar->append(*nav("software-update-available-symbolic", "Updates", "updates"));

        sidebar->append(*label("BUILD", {"sidebar-section"}));
        sidebar->append(*nav("tools-build-symbolic", "Build Queue", "build-queue"));

        sidebar->append(*label("SYSTEM", {"sidebar-section"}));
        sidebar->append(*nav("preferences-system-symbolic", "Settings", "settings"));

        auto* spacer = Gtk::make_managed<Gtk::Box>(); spacer->set_vexpand(true); sidebar->append(*spacer);

        auto* status_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        status_box->set_css_classes({"status-indicator"});
        daemon_state_ = label("●  Ready", {"status-ok"}); daemon_state_->set_max_width_chars(16);
        daemon_state_->set_ellipsize(Pango::EllipsizeMode::END);
        status_box->append(*daemon_state_); sidebar->append(*status_box);

        auto* sidebar_frame = Gtk::make_managed<Gtk::ScrolledWindow>();
        sidebar_frame->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::NEVER);
        sidebar_frame->set_min_content_width(164); sidebar_frame->set_max_content_width(164);
        sidebar_frame->set_size_request(164, -1);
        sidebar_frame->set_child(*sidebar); layout->append(*sidebar_frame);

        // Page stack
        stack_ = Gtk::make_managed<Gtk::Stack>();
        stack_->set_transition_type(Gtk::StackTransitionType::CROSSFADE);
        stack_->set_hexpand(true); stack_->set_vexpand(true);

        stack_->add(*build_overview(), "overview");
        stack_->add(*build_search_page(), "search");
        stack_->add(*build_updates_page(), "updates");
        stack_->add(*build_build_queue_page(), "build-queue");
        stack_->add(*build_settings_page(), "settings");

        layout->append(*stack_); set_child(*layout);
        navigate("overview");
    }

    // ── Overview Page ──────────────────────────────────────────────

    Gtk::Box* build_overview() {
        auto* result = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 20);
        result->set_css_classes({"content"});

        auto* hero = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 24);
        hero->set_css_classes({"hero"});
        auto* hero_icon = Gtk::make_managed<Gtk::Image>();
        hero_icon->set_from_icon_name("package-installed-symbolic");
        hero_icon->set_pixel_size(42); hero_icon->set_css_classes({"hero-icon"});
        hero->append(*hero_icon);
        auto* hero_copy = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        hero_copy->set_hexpand(true);
        hero_copy->append(*label("PACKAGE STATUS", {"hero-kicker"}));
        hero_title_ = label("System packages up to date", {"hero-title"});
        hero_copy->append(*hero_title_);
        hero_copy->append(*label("Search, install, and build packages from repos and AUR.", {"hero-copy"}));
        hero->append(*hero_copy); result->append(*hero);

        auto* stats = Gtk::make_managed<Gtk::Grid>();
        stats->set_column_spacing(14); stats->set_column_homogeneous(true);
        stats->attach(*stat_card("Installed", installed_), 0, 0);
        stats->attach(*stat_card("Available updates", update_count_), 1, 0);
        stats->attach(*stat_card("AUR out-of-date", aur_ootd_), 2, 0);
        stats->attach(*stat_card("Build queue", build_count_), 3, 0);
        result->append(*stats);

        auto* quick = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 9);
        quick->set_css_classes({"quick-actions"});
        auto shortcut = [this, quick](std::string const& icon, std::string const& title, std::string const& target, bool primary = false) {
            auto* btn = Gtk::make_managed<Gtk::Button>(title);
            btn->set_icon_name(icon);
            btn->set_css_classes({primary ? "primary" : "secondary"});
            btn->set_hexpand(true);
            btn->signal_clicked().connect([this, target] { navigate(target); });
            quick->append(*btn);
        };
        shortcut("system-search-symbolic", "Search packages", "search", true);
        shortcut("software-update-available-symbolic", "Check updates", "updates");
        shortcut("tools-build-symbolic", "Build queue", "build-queue");
        result->append(*quick);

        Gtk::Box* ootd_body = nullptr;
        auto* ootd_card = page("Out-of-date AUR packages", "Packages installed from AUR that have newer versions available", ootd_body);
        ootd_list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        ootd_body->append(*ootd_list_); result->append(*ootd_card);

        return result;
    }

    // ── Updates Page ───────────────────────────────────────────────

    Gtk::Box* build_updates_page() {
        Gtk::Box* body = nullptr;
        auto* result = page("System updates", "Review changes before allowing a system upgrade", body);

        auto* toolbar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        toolbar->set_css_classes({"card", "update-toolbar"});
        check_updates_btn_ = Gtk::make_managed<Gtk::Button>("Check now");
        check_updates_btn_->set_icon_name("view-refresh-symbolic");
        check_updates_btn_->set_css_classes({"secondary"});
        check_updates_btn_->signal_clicked().connect([this] { check_for_updates(); });
        toolbar->append(*check_updates_btn_);
        update_summary_ = label("Updates have not been checked yet", {"dim"});
        update_summary_->set_hexpand(true); toolbar->append(*update_summary_);
        body->append(*toolbar);

        updates_list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 7);
        updates_list_->append(*empty_state("Ready when you are", "Check for updates to compare installed packages with your repository databases."));
        body->append(*updates_list_);

        auto* approval = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        approval->set_css_classes({"approval-card"});
        approve_updates_ = Gtk::make_managed<Gtk::CheckButton>("I reviewed these packages and approve the update");
        approve_updates_->set_hexpand(true);
        approve_updates_->signal_toggled().connect([this] {
            apply_updates_btn_->set_sensitive(approve_updates_->get_active() && !available_updates_.empty());
        });
        approval->append(*approve_updates_);
        apply_updates_btn_ = Gtk::make_managed<Gtk::Button>("Update system");
        apply_updates_btn_->set_icon_name("software-update-available-symbolic");
        apply_updates_btn_->set_css_classes({"primary"}); apply_updates_btn_->set_sensitive(false);
        apply_updates_btn_->signal_clicked().connect([this] { apply_updates(); });
        approval->append(*apply_updates_btn_); body->append(*approval);
        return result;
    }

    void check_for_updates() {
        if (checking_updates_) return;
        checking_updates_ = true; check_updates_btn_->set_sensitive(false);
        update_summary_->set_text("Checking installed packages…"); update_status("Checking updates…");
        std::thread([this] {
            try {
                auto updates = backend_->get_available_updates();
                Glib::signal_idle().connect_once([this, updates = std::move(updates)]() mutable {
                    available_updates_ = std::move(updates); checking_updates_ = false;
                    check_updates_btn_->set_sensitive(true); approve_updates_->set_active(false);
                    clear(*updates_list_);
                    last_check_ = std::time(nullptr); save_update_preferences(check_interval_hours_, last_check_);
                    if (available_updates_.empty()) {
                        updates_list_->append(*empty_state("You're up to date", "No repository updates are currently available."));
                        update_summary_->set_text("Checked just now · no updates");
                    } else {
                        for (auto const& update : available_updates_) {
                            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
                            row->set_css_classes({"row-card", "update-row"});
                            auto* icon = Gtk::make_managed<Gtk::Image>(); icon->set_from_icon_name("package-x-generic-symbolic");
                            auto* copy = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 3); copy->set_hexpand(true);
                            copy->append(*label(update.name, {"package-name"}));
                            copy->append(*label(update.installed_version + "  →  " + update.available_version, {"package-version"}));
                            auto* badge = label("REPOSITORY", {"badge", "repo"});
                            row->append(*icon); row->append(*copy); row->append(*badge); updates_list_->append(*row);
                        }
                        update_summary_->set_text(std::to_string(available_updates_.size()) + " updates available");
                    }
                    update_count_->set_text(std::to_string(available_updates_.size()));
                    hero_title_->set_text(available_updates_.empty() ? "Your system is up to date" :
                        std::to_string(available_updates_.size()) + " updates need your review");
                    update_status(available_updates_.empty() ? "System up to date" : "Updates available");
                });
            } catch (std::exception const& e) {
                Glib::signal_idle().connect_once([this, msg = std::string(e.what())] {
                    checking_updates_ = false; check_updates_btn_->set_sensitive(true);
                    update_summary_->set_text("Update check failed: " + msg); update_status("Update check failed");
                });
            }
        }).detach();
    }

    void apply_updates() {
        if (!approve_updates_->get_active() || available_updates_.empty()) return;
        apply_updates_btn_->set_sensitive(false); check_updates_btn_->set_sensitive(false);
        update_summary_->set_text("System update is running…"); update_status("Updating system…");
        std::thread([this] {
            int status = -1;
            try { status = backend_->apply_system_updates(); } catch (...) {}
            Glib::signal_idle().connect_once([this, status] {
                check_updates_btn_->set_sensitive(true);
                update_summary_->set_text(status == 0 ? "Update completed successfully" : "Update failed or was cancelled");
                update_status(status == 0 ? "Update complete" : "Update failed");
                if (status == 0) check_for_updates();
                else apply_updates_btn_->set_sensitive(approve_updates_->get_active());
            });
        }).detach();
    }

    void check_updates_if_due() {
        if (check_interval_hours_ <= 0) return;
        auto elapsed = std::time(nullptr) - last_check_;
        if (last_check_ == 0 || elapsed >= check_interval_hours_ * 3600) check_for_updates();
    }

    void start_schedule_timer() {
        schedule_connection_ = Glib::signal_timeout().connect_seconds([this] { check_updates_if_due(); return true; }, 60);
    }

    void refresh_overview() {
        try {
            auto local_pkgs = alpm::get_local_packages();
            installed_->set_text(std::to_string(local_pkgs.size()));

            int ootd_count = 0;
            for (auto& pkg : local_pkgs) {
                try {
                    auto sync_pkg = alpm::get_sync_package(pkg.name);
                    if (sync_pkg) continue;
                } catch (...) {}
                if (aur_cache::has(pkg.name)) {
                    auto info = aur_cache::check_ootd(pkg.name, pkg.version);
                    if (info.is_out_of_date) ++ootd_count;
                }
            }
            aur_ootd_->set_text(std::to_string(ootd_count));
        } catch (...) {
            installed_->set_text("?");
            aur_ootd_->set_text("?");
        }
    }

    // ── Search Page ────────────────────────────────────────────────

    Gtk::Box* build_search_page() {
        Gtk::Box* list_body = nullptr;
        auto* list_page = page("Search packages", "Search repositories and the AUR", list_body);

        auto* toolbar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 9);
        toolbar->set_css_classes({"card"});
        search_entry_ = Gtk::make_managed<Gtk::SearchEntry>();
        search_entry_->set_placeholder_text("Search packages…"); search_entry_->set_hexpand(true);
        search_entry_->signal_search_changed().connect([this] { do_search(); });
        toolbar->append(*search_entry_);

        auto* refresh_btn = Gtk::make_managed<Gtk::Button>("Refresh");
        refresh_btn->set_css_classes({"secondary"});
        refresh_btn->signal_clicked().connect([this] { refresh_cache_and_search(); });
        toolbar->append(*refresh_btn);
        list_body->append(*toolbar);

        results_scroll_ = Gtk::make_managed<Gtk::ScrolledWindow>(); results_scroll_->set_vexpand(true);
        results_list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        results_list_->append(*empty_state("Find your next package", "Search by package name or description across official repositories and the AUR."));
        results_scroll_->set_child(*results_list_); list_body->append(*results_scroll_);

        // Detail pane
        Gtk::Box* detail_body = nullptr;
        auto* detail_page = page("Package details", "", detail_body);
        detail_body->set_css_classes({"detail-pane"});
        detail_icon_ = Gtk::make_managed<Gtk::Image>(); detail_icon_->set_pixel_size(32);
        detail_body->append(*detail_icon_);
        detail_name_ = label("", {"detail-title"}); detail_body->append(*detail_name_);
        detail_version_ = label("", {"meta"}); detail_body->append(*detail_version_);
        detail_origin_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        detail_body->append(*detail_origin_);
        detail_desc_ = label("", {}); detail_desc_->set_wrap(true); detail_body->append(*detail_desc_);

        build_btn_ = Gtk::make_managed<Gtk::Button>("Add to build queue");
        build_btn_->set_css_classes({"primary"}); build_btn_->set_visible(false);
        build_btn_->signal_clicked().connect([this] { add_selected_to_queue(); });
        detail_body->append(*build_btn_);

        auto* split = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
        split->set_position(500); split->set_vexpand(true);
        split->set_resize_start_child(true); split->set_resize_end_child(true);
        split->set_start_child(*list_page); split->set_end_child(*detail_page);

        auto* result = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        result->append(*split);
        return result;
    }

    void do_search() {
        auto query = search_entry_ ? search_entry_->get_text().raw() : "";
        if (query.empty()) { clear(*results_list_); results_list_->append(*empty_state("Search packages", "Type a package name to search repos and AUR")); return; }

        searching_ = true; update_status("Searching…");

        std::thread([this, query]() {
            try {
                auto results = backend_->search(query, 50);
                Glib::signal_idle().connect_once([this, results = std::move(results)]() {
                    searching_ = false;
                    clear(*results_list_);
                    if (results.empty()) { results_list_->append(*empty_state("No results", "Try a different search term")); return; }

                    for (auto& r : results) {
                        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
                        row->set_css_classes({"row-card", "package-row"});
                        row->set_valign(Gtk::Align::START);

                        auto* dot = Gtk::make_managed<Gtk::Box>();
                        std::vector<Glib::ustring> dot_classes = {"status-dot"};
                        if (r.origin == "aur") dot_classes.push_back("aur");
                        else dot_classes.push_back("repo");
                        if (r.is_out_of_date) { dot_classes.clear(); dot_classes = {"status-dot", "out-of-date"}; }
                        dot->set_css_classes(dot_classes);

                        auto* info = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
                        info->set_hexpand(true);
                        info->append(*label(r.name, {"package-name"}));
                        auto* ver = label(r.version, {"package-version"});
                        ver->set_ellipsize(Pango::EllipsizeMode::END);
                        info->append(*ver);
                        if (!r.description.empty()) {
                            auto* desc = label(r.description, {"package-desc"});
                            desc->set_ellipsize(Pango::EllipsizeMode::END);
                            desc->set_max_width_chars(58);
                            info->append(*desc);
                        }

                        auto* badge = Gtk::make_managed<Gtk::Label>();
                        std::vector<Glib::ustring> bc = {"badge"};
                        if (r.origin == "aur") { bc.push_back("aur"); badge->set_text("AUR"); }
                        else { bc.push_back("repo"); badge->set_text(r.origin); }
                        badge->set_css_classes(bc);

                        if (r.is_out_of_date) {
                            auto* ootd = Gtk::make_managed<Gtk::Label>("Out of date");
                            ootd->set_css_classes({"badge", "out-of-date"});
                            info->append(*ootd);
                        }

                        row->append(*dot); row->append(*info); row->append(*badge);

                        auto result_copy = r;
                        auto gesture = Gtk::GestureClick::create();
                        gesture->set_button(0);
                        gesture->signal_pressed().connect([this, result_copy](int n_press, double x, double y) {
                            if (n_press == 1) show_package_detail(result_copy);
                        });
                        row->add_controller(gesture);

                        results_list_->append(*row);
                    }

                    update_status(std::to_string(results.size()) + " packages found");
                });
            } catch (std::exception const& e) {
                Glib::signal_idle().connect_once([this, msg = std::string(e.what())]() {
                    searching_ = false; clear(*results_list_);
                    results_list_->append(*empty_state("Search error", msg));
                    update_status("Search error");
                });
            }
        }).detach();
    }

    void refresh_cache_and_search() {
        std::thread([this]() {
            backend_->refresh_cache();
            if (search_entry_ && !search_entry_->get_text().raw().empty()) do_search();
            else Glib::signal_idle().connect_once([this]() { update_status("Cache refreshed"); });
        }).detach();
    }

    void show_package_detail(SearchResult const& r) {
        detail_name_->set_text(r.name);
        detail_version_->set_text(r.version);
        detail_desc_->set_text(r.description.empty() ? "No description available" : r.description);

        clear(*detail_origin_);
        auto* badge = Gtk::make_managed<Gtk::Label>();
        std::vector<Glib::ustring> bc = {"badge"};
        if (r.origin == "aur") { bc.push_back("aur"); badge->set_text("AUR"); }
        else { bc.push_back("repo"); badge->set_text(r.origin); }
        badge->set_css_classes(bc); detail_origin_->append(*badge);

        if (r.is_out_of_date) {
            auto* ootd = Gtk::make_managed<Gtk::Label>("Out of date in AUR");
            ootd->set_css_classes({"badge", "out-of-date"}); detail_origin_->append(*ootd);
        }

        build_btn_->set_visible(r.origin == "aur");
    }

    void add_selected_to_queue() {
        update_status("Added to build queue");
        navigate("build-queue");
    }

    // ── Build Queue Page ───────────────────────────────────────────

    Gtk::Box* build_build_queue_page() {
        Gtk::Box* body = nullptr; auto* result = page("Build queue", "Packages queued for building from PKGBUILD", body);

        auto* toolbar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 9);
        start_build_btn_ = Gtk::make_managed<Gtk::Button>("Start build");
        start_build_btn_->set_css_classes({"primary"});
        start_build_btn_->signal_clicked().connect([this] { start_build(); });
        toolbar->append(*start_build_btn_);

        cancel_build_btn_ = Gtk::make_managed<Gtk::Button>("Cancel");
        cancel_build_btn_->set_css_classes({"secondary"}); cancel_build_btn_->set_visible(false);
        cancel_build_btn_->signal_clicked().connect([this] { cancel_build(); });
        toolbar->append(*cancel_build_btn_);
        result->append(*toolbar);

        auto* progress_card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        progress_card->set_css_classes({"card"});
        build_status_label_ = label("No build in progress", {});
        progress_bar_ = Gtk::make_managed<Gtk::ProgressBar>();
        progress_bar_->set_fraction(0.0);
        progress_graph_ = Gtk::make_managed<ProgressGraph>();
        progress_card->append(*build_status_label_);
        progress_card->append(*progress_bar_);
        progress_card->append(*progress_graph_);
        body->append(*progress_card);

        auto* log_card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        log_card->set_css_classes({"card"});
        log_card->append(*label("Build output", {"field-label"}));
        auto* log_scroll = Gtk::make_managed<Gtk::ScrolledWindow>(); log_scroll->set_vexpand(true);
        log_view_ = Gtk::make_managed<Gtk::TextView>();
        log_view_->set_editable(false); log_view_->set_wrap_mode(Gtk::WrapMode::WORD);
        log_view_->set_css_classes({"log-view"});
        log_buffer_ = log_view_->get_buffer();
        log_scroll->set_child(*log_view_);
        log_card->append(*log_scroll);
        body->append(*log_card);

        return result;
    }

    void start_build() {
        if (backend_->is_building()) return;

        start_build_btn_->set_visible(false);
        cancel_build_btn_->set_visible(true);
        build_status_label_->set_text("Building…");
        update_status("Build started");

        auto progress_cb = [this](int percent, std::string const& status) {
            Glib::signal_idle().connect_once([this, percent, status]() {
                if (!progress_bar_ || !build_status_label_ || !progress_graph_) return;
                progress_bar_->set_fraction(percent / 100.0);
                build_status_label_->set_text(status);
                update_status(status);
                progress_graph_->record(percent / 100.0);
            });
        };

        auto log_cb = [this](std::string const& line) {
            Glib::signal_idle().connect_once([this, line]() {
                if (!log_view_ || !log_buffer_) return;
                auto iter = (*log_buffer_).end();
                (*log_buffer_).insert(iter, line + "\n");
                log_view_->scroll_to(iter);
            });
        };

        std::vector<SearchResult> targets;
        backend_->start_build(targets, progress_cb, log_cb);
    }

    void cancel_build() {
        backend_->cancel_build();
        start_build_btn_->set_visible(true);
        cancel_build_btn_->set_visible(false);
        build_status_label_->set_text("Build cancelled");
        update_status("Build cancelled");
    }

    // ── Settings Page ──────────────────────────────────────────────

    Gtk::Box* build_settings_page() {
        Gtk::Box* body = nullptr; auto* result = page("Settings", "Update schedule, safety, and appearance", body);

        auto* update_card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        update_card->set_css_classes({"settings-card"});
        update_card->append(*label("Automatic update checks", {"rule-description"}));
        update_card->append(*label("pacmkr only checks in the background. Installing updates always requires your explicit approval.", {"dim"}));
        auto* interval = Gtk::make_managed<Gtk::ComboBoxText>();
        interval->append("0", "Never"); interval->append("1", "Every hour");
        interval->append("6", "Every 6 hours"); interval->append("24", "Daily");
        interval->set_active_id(std::to_string(check_interval_hours_));
        interval->signal_changed().connect([this, interval] {
            try { check_interval_hours_ = std::stoi(interval->get_active_id()); } catch (...) { check_interval_hours_ = 24; }
            save_update_preferences(check_interval_hours_, last_check_);
            update_status(check_interval_hours_ == 0 ? "Scheduled checks disabled" : "Update schedule saved");
            check_updates_if_due();
        });
        update_card->append(*interval); body->append(*update_card);

        auto* app_card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        app_card->set_css_classes({"settings-card"});
        app_card->append(*label("Appearance", {"rule-description"}));
        app_card->append(*label("Choose a color palette, or follow your desktop's light/dark setting.", {"dim"}));

        auto* theme_combo = Gtk::make_managed<Gtk::ComboBoxText>();
        theme_combo->append("system", "System");
        theme_combo->append("light", "Light");
        theme_combo->append("dark", "Dark");
        theme_combo->set_active_id(appearance_);
        theme_combo->signal_changed().connect([this, theme_combo] { apply_appearance(theme_combo->get_active_id()); });
        app_card->append(*theme_combo);
        body->append(*app_card);

        auto* about_card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        about_card->set_css_classes({"settings-card"});
        about_card->append(*label("About", {"rule-description"}));
        auto* version = label("pacmkr " PACKAGE_VERSION, {"meta"}); version->set_halign(Gtk::Align::START);
        about_card->append(*version);
        about_card->append(*label("A pacman drop-in replacement with AUR integration.", {"dim"}));
        body->append(*about_card);

        return result;
    }

    // ── Status ─────────────────────────────────────────────────────

    void update_status(std::string const& status) {
        if (daemon_state_) daemon_state_->set_text("●  " + status);
    }

    // ── State ──────────────────────────────────────────────────────

    std::shared_ptr<Backend> backend_ = std::make_shared<Backend>();
    std::string appearance_{"system"};
    std::unique_ptr<ThemeWatcher> theme_watcher_;

    Gtk::ToggleButton* nav_group_{nullptr};
    std::unordered_map<std::string, Gtk::ToggleButton*> nav_buttons_;
    Gtk::Stack* stack_{nullptr};

    // Overview
    Gtk::Label* installed_{nullptr};
    Gtk::Label* update_count_{nullptr};
    Gtk::Label* aur_ootd_{nullptr};
    Gtk::Label* build_count_{nullptr};
    Gtk::Label* hero_title_{nullptr};
    Gtk::Box* ootd_list_{nullptr};

    // Search
    Gtk::SearchEntry* search_entry_{nullptr};
    Gtk::ScrolledWindow* results_scroll_{nullptr};
    Gtk::Box* results_list_{nullptr};
    Gtk::Image* detail_icon_{nullptr};
    Gtk::Label* detail_name_{nullptr};
    Gtk::Label* detail_version_{nullptr};
    Gtk::Box* detail_origin_{nullptr};
    Gtk::Label* detail_desc_{nullptr};
    Gtk::Button* build_btn_{nullptr};
    bool searching_{false};

    // Updates
    Gtk::Button* check_updates_btn_{nullptr};
    Gtk::Label* update_summary_{nullptr};
    Gtk::Box* updates_list_{nullptr};
    Gtk::CheckButton* approve_updates_{nullptr};
    Gtk::Button* apply_updates_btn_{nullptr};
    std::vector<AvailableUpdate> available_updates_;
    bool checking_updates_{false};
    int check_interval_hours_{24};
    std::time_t last_check_{0};
    sigc::connection schedule_connection_;

    // Build queue
    Gtk::Button* start_build_btn_{nullptr};
    Gtk::Button* cancel_build_btn_{nullptr};
    Gtk::ProgressBar* progress_bar_{nullptr};
    ProgressGraph* progress_graph_{nullptr};
    Gtk::Label* build_status_label_{nullptr};
    Gtk::TextView* log_view_{nullptr};
    Glib::RefPtr<Gtk::TextBuffer> log_buffer_;

    // Status
    Gtk::Label* daemon_state_{nullptr};

    NotificationSink notify_;
};

} // anonymous namespace

// ── Entry point ────────────────────────────────────────────────────

int run_gtk_gui(int argc, char* argv[]) {
    auto app = Gtk::Application::create("com.pacmkr.gui");
    return app->make_window_and_run<MainWindow>(argc, argv, app);
}

} // namespace pacmkr::gui

int main(int argc, char* argv[]) {
    // Check for display availability — gracefully exit if none
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
        std::cerr << "No display available (set DISPLAY or WAYLAND_DISPLAY).\n";
        return 1;
    }
    return pacmkr::gui::run_gtk_gui(argc, argv);
}
