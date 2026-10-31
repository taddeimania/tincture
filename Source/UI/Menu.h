// Menu.h — Tincture's menus.
//
// Every dropdown, context menu and sub-menu in the panel goes through here.
// They are drawn INSIDE the plugin panel (a layer on top of everything else)
// rather than as separate OS windows, which is what juce::PopupMenu does.
// That buys us:
//   - menus scale with the panel (the panel is a fixed design space scaled
//     to the window, and separate windows ignore that transform);
//   - they never open behind the plugin window, off-screen, or on the wrong
//     monitor, and the host never steals their focus;
//   - one place that decides placement, scrolling, columns and keyboard
//     handling, so every menu behaves the same.
//
// Rules this file guarantees (and Tests/MenuTests.cpp checks):
//   1. Opening never selects anything. An item fires only on a mouse-up that
//      followed a mouse-down on that same item, or on Return.
//   2. A menu's action runs AFTER the menu has closed, asynchronously, and
//      only if the component that opened it still exists. Actions are free
//      to rebuild the UI (including the component that opened the menu).
//   3. Clicking outside closes the menu and the click goes nowhere else, so
//      clicking the dropdown that opened a menu just closes it.
//   4. If the component that opened the menu disappears (preset load,
//      view change, editor closing), the menu closes without firing.
//   5. A menu never leaves the panel: it flips above its anchor when there
//      is no room below, then splits into columns, then scrolls.
//   6. The current value (ticked item) is highlighted and scrolled into
//      view when the menu opens, so arrow keys move from it.
//   7. Sub-menus open on hover after a short delay, or at once on click or
//      Right-arrow. Moving diagonally towards an open sub-menu across other
//      rows doesn't close it.
//   8. Long menus can be searched: type to filter (every searchable menu
//      shows a filter row); otherwise typing a letter jumps to the next
//      item starting with it.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>

namespace tincture::ui
{
class Menu
{
public:
    struct Item
    {
        enum class Kind { Action, Separator, Header, Submenu, ColumnBreak };
        Kind kind = Kind::Action;
        juce::String text, shortcut, detail;      // detail: dim text after the label
        bool ticked = false, enabled = true;
        bool keepOpen = false;                      // run at once and keep the menu open (multi-select)
        std::function<bool()> tickedFn;             // live tick state, for keepOpen items
        juce::Colour swatch { 0 };                  // a small colour square before the text
        std::function<void()> action;
        std::shared_ptr<Menu> sub;

        bool isTicked() const { return tickedFn ? tickedFn() : ticked; }
        bool selectable() const { return enabled && (kind == Kind::Action || kind == Kind::Submenu); }
    };

    Menu() = default;

    Menu& add (const juce::String& text, std::function<void()> action, bool ticked = false, bool enabled = true);
    Menu& add (Item item);
    Menu& toggle (const juce::String& text, bool state, std::function<void()> action, bool enabled = true);
    Menu& separator();
    Menu& header (const juce::String& text);
    Menu& sub (const juce::String& text, Menu menu, bool ticked = false, bool enabled = true);
    Menu& columnBreak();

    // Shortcut text (drawn dim, right-aligned) / dim detail / swatch for the LAST added item.
    Menu& withShortcut (const juce::String&);
    Menu& withDetail (const juce::String&);
    Menu& withSwatch (juce::Colour);
    Menu& keepingOpen (std::function<bool()> tickedFn);

    Menu& searchable (bool s = true) { search = s; return *this; }
    Menu& minimumWidth (int w) { minWidth = w; return *this; }
    Menu& title (const juce::String& t) { heading = t; return *this; }

    const std::vector<Item>& items() const noexcept { return list; }
    bool empty() const noexcept { return list.empty(); }
    bool isSearchable() const noexcept { return search || countLeaves() > 40; }
    int getMinimumWidth() const noexcept { return minWidth; }
    const juce::String& getTitle() const noexcept { return heading; }
    int countLeaves() const;

    // Every action item, depth first, with its sub-menu path ("Analog > MG Low 12").
    struct Leaf { juce::String path; const Item* item; };
    void collectLeaves (std::vector<Leaf>&, const juce::String& prefix = {}) const;

    // Find an item by its label path ("Analog/MG Low 12"); for tests and automation.
    const Item* find (const juce::String& slashPath) const;

private:
    std::vector<Item> list;
    bool search = false;
    int minWidth = 0;
    juce::String heading;
};

struct MenuOptions
{
    enum class Place { Below, Above, Right, AtPoint };
    Place place = Place::Below;
    juce::Point<int> point;        // AtPoint: in the anchor's coordinates
    int minWidth = 0;              // 0: at least the anchor's width (Below/Above)
    bool matchAnchorWidth = true;
};

// Open `menu` from `anchor`. Any menu already open in that panel closes first.
void showMenu (juce::Component& anchor, Menu menu, MenuOptions options = {});
// A context menu at a point in `anchor` (typically the right-click position).
void showContextMenu (juce::Component& anchor, juce::Point<int> where, Menu menu);
void dismissMenus (juce::Component& anyComponentInPanel);
bool isMenuOpen (juce::Component& anyComponentInPanel);

class MenuLayer;

// The top-level panel derives from this; menus live in a layer on top of it.
class MenuHost : public juce::Component
{
public:
    MenuHost();
    ~MenuHost() override;
    MenuLayer& getMenuLayer();
    bool hasMenuLayer() const noexcept { return layer != nullptr; }
    void resizeMenuLayer();

private:
    std::unique_ptr<MenuLayer> layer;
};

// ---------------------------------------------------------------------------
// Implementation detail, public for the tests.
// ---------------------------------------------------------------------------
class MenuPane;

class MenuLayer final : public juce::Component, private juce::Timer
{
public:
    MenuLayer();
    ~MenuLayer() override;

    void open (juce::Component& anchor, Menu menu, MenuOptions options);
    void dismiss();
    bool isOpen() const noexcept { return ! panes.empty(); }
    int paneCount() const noexcept { return (int) panes.size(); }
    MenuPane* pane (int i) const { return juce::isPositiveAndBelow (i, (int) panes.size()) ? panes[(size_t) i].get() : nullptr; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool hitTest (int, int) override { return isOpen(); }

    // Called by panes.
    void openSubmenu (MenuPane& parent, int row);
    void closeAbove (int depth);
    void choose (const Menu::Item& item);
    void runKeepOpen (const Menu::Item& item);
    int depthOf (const MenuPane&) const;
    juce::Rectangle<int> available() const;

    // Diagonal hover protection: is `p` heading for the open child pane of `parent`?
    bool headingIntoChild (const MenuPane& parent, juce::Point<int> p) const;

    // Last action fired (tests).
    juce::String lastChosen;

private:
    void timerCallback() override;
    std::vector<std::unique_ptr<MenuPane>> panes;
    juce::Component::SafePointer<juce::Component> anchor;
    juce::Point<int> lastMouse, prevMouse;
    friend class MenuPane;
};

class MenuPane final : public juce::Component, private juce::Timer
{
public:
    MenuPane (MenuLayer&, std::shared_ptr<Menu>, int depth);

    // Lays the menu out to fit `area` (layer coordinates); `target` is what
    // it opens from, `place` how.
    void place (juce::Rectangle<int> target, MenuOptions::Place place, int minWidth, juce::Rectangle<int> area);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent& e) override { mouseMove (e); }
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent& e) override { mouseMove (e); }
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    bool key (const juce::KeyPress&);       // returns true if handled

    // Rows as laid out: index into `rows`, which are the visible items (the
    // menu's items, or the search matches).
    struct Row
    {
        const Menu::Item* item = nullptr;
        juce::String path;                  // search results show where an item lives
        juce::Rectangle<int> bounds;        // in pane coordinates, before scrolling
    };
    const std::vector<Row>& getRows() const noexcept { return rows; }
    int rowAt (juce::Point<int> paneLocal) const;
    int findRow (const juce::String& text) const;
    juce::Rectangle<int> rowBoundsOnScreen (int row) const;   // pane coordinates, after scrolling
    int hotRow() const noexcept { return hot; }
    int openRow() const noexcept { return childRow; }
    void setHot (int row, bool fromKeyboard);
    const juce::String& filterText() const noexcept { return filter; }
    int scrollOffset() const noexcept { return scroll; }
    int columnCount() const noexcept { return columns; }
    std::shared_ptr<Menu> getMenu() const { return menu; }
    void setChildRow (int r) { childRow = r; repaint(); }

private:
    void timerCallback() override;
    void layoutRows();
    void rebuildRows();
    void ensureVisible (int row);
    int moveHot (int from, int delta) const;
    int contentTop() const;
    juce::Rectangle<int> viewport() const;
    int contentHeight = 0;

    MenuLayer& layer;
    std::shared_ptr<Menu> menu;
    std::vector<Row> rows;
    int hot = -1, pressedRow = -1, childRow = -1, pendingRow = -1;
    int scroll = 0, maxScroll = 0, columns = 1, colWidth = 200;
    bool pendingOpen = false;
    juce::String filter;
    juce::Rectangle<int> fitArea, targetArea;
    MenuOptions::Place placement = MenuOptions::Place::Below;
    int wantMinWidth = 0;
};
} // namespace tincture::ui
