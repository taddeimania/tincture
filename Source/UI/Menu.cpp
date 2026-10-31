#include "Menu.h"
#include "Widgets.h"

namespace tincture::ui
{
using namespace juce;

namespace
{
constexpr int kRowH = 22, kHeaderH = 20, kSepH = 9, kFilterH = 24, kTitleH = 22;
constexpr int kPadX = 8, kTickW = 18, kArrowW = 16, kBorder = 2, kMargin = 6;
constexpr int kMaxColWidth = 340, kMinColWidth = 120;
constexpr int kOpenDelayMs = 140, kSwitchDelayMs = 260;

Font rowFont()    { return panelFont (14.0f, false); }
Font headerFont() { return panelFont (11.5f, true); }

int textWidth (const Font& f, const String& s) { return (int) std::ceil (GlyphArrangement::getStringWidth (f, s)); }

int rowHeight (const Menu::Item& it)
{
    switch (it.kind)
    {
        case Menu::Item::Kind::Separator:   return kSepH;
        case Menu::Item::Kind::Header:      return kHeaderH;
        case Menu::Item::Kind::ColumnBreak: return 0;
        case Menu::Item::Kind::Action:
        case Menu::Item::Kind::Submenu:     return kRowH;
    }
    return kRowH;
}

MenuLayer* layerFor (Component& c, bool create)
{
    if (auto* host = dynamic_cast<MenuHost*> (&c))
        return create || host->hasMenuLayer() ? &host->getMenuLayer() : nullptr;
    if (auto* host = c.findParentComponentOfClass<MenuHost>())
        return create || host->hasMenuLayer() ? &host->getMenuLayer() : nullptr;
    return nullptr;
}

void drawTick (Graphics& g, Rectangle<float> r, Colour c)
{
    Path p;
    const auto b = r.withSizeKeepingCentre (9.0f, 9.0f);
    p.startNewSubPath (b.getX(), b.getCentreY());
    p.lineTo (b.getX() + b.getWidth() * 0.38f, b.getBottom());
    p.lineTo (b.getRight(), b.getY());
    g.setColour (c);
    g.strokePath (p, PathStrokeType (2.0f, PathStrokeType::mitered, PathStrokeType::square));
}

void drawArrow (Graphics& g, Rectangle<float> r, Colour c, bool right = true)
{
    Path p;
    const auto cx = r.getCentreX(), cy = r.getCentreY();
    if (right) p.addTriangle (cx - 2.5f, cy - 4.5f, cx - 2.5f, cy + 4.5f, cx + 3.0f, cy);
    else       p.addTriangle (cx - 4.5f, cy - 2.5f, cx + 4.5f, cy - 2.5f, cx, cy + 3.0f);
    g.setColour (c);
    g.fillPath (p);
}
} // namespace

// ===========================================================================
// Menu
// ===========================================================================
Menu& Menu::add (const String& text, std::function<void()> action, bool ticked, bool enabled)
{
    Item it;
    it.text = text;
    it.action = std::move (action);
    it.ticked = ticked;
    it.enabled = enabled;
    list.push_back (std::move (it));
    return *this;
}

Menu& Menu::add (Item item) { list.push_back (std::move (item)); return *this; }

Menu& Menu::toggle (const String& text, bool state, std::function<void()> action, bool enabled)
{
    return add (text, std::move (action), state, enabled);
}

Menu& Menu::separator()
{
    // No leading or doubled separators.
    if (! list.empty() && list.back().kind != Item::Kind::Separator && list.back().kind != Item::Kind::ColumnBreak)
    {
        Item it;
        it.kind = Item::Kind::Separator;
        list.push_back (std::move (it));
    }
    return *this;
}

Menu& Menu::header (const String& text)
{
    Item it;
    it.kind = Item::Kind::Header;
    it.text = text;
    list.push_back (std::move (it));
    return *this;
}

Menu& Menu::sub (const String& text, Menu m, bool ticked, bool enabled)
{
    Item it;
    it.kind = Item::Kind::Submenu;
    it.text = text;
    it.ticked = ticked;
    it.enabled = enabled && ! m.empty();
    it.sub = std::make_shared<Menu> (std::move (m));
    list.push_back (std::move (it));
    return *this;
}

Menu& Menu::columnBreak()
{
    Item it;
    it.kind = Item::Kind::ColumnBreak;
    list.push_back (std::move (it));
    return *this;
}

Menu& Menu::withShortcut (const String& s) { if (! list.empty()) list.back().shortcut = s; return *this; }
Menu& Menu::withDetail (const String& s)   { if (! list.empty()) list.back().detail = s; return *this; }
Menu& Menu::withSwatch (Colour c)          { if (! list.empty()) list.back().swatch = c; return *this; }
Menu& Menu::keepingOpen (std::function<bool()> fn)
{
    if (! list.empty()) { list.back().keepOpen = true; list.back().tickedFn = std::move (fn); }
    return *this;
}

int Menu::countLeaves() const
{
    int n = 0;
    for (const auto& it : list)
    {
        if (it.kind == Item::Kind::Action) ++n;
        else if (it.kind == Item::Kind::Submenu && it.sub) n += it.sub->countLeaves();
    }
    return n;
}

void Menu::collectLeaves (std::vector<Leaf>& out, const String& prefix) const
{
    for (const auto& it : list)
    {
        if (it.kind == Item::Kind::Action)
            out.push_back ({ prefix.isEmpty() ? String() : prefix, &it });
        else if (it.kind == Item::Kind::Submenu && it.sub && it.enabled)
            it.sub->collectLeaves (out, prefix.isEmpty() ? it.text : prefix + " > " + it.text);
    }
}

const Menu::Item* Menu::find (const String& slashPath) const
{
    const auto first = slashPath.upToFirstOccurrenceOf ("/", false, false);
    const auto rest = slashPath.fromFirstOccurrenceOf ("/", false, false);
    for (const auto& it : list)
    {
        if (it.text != first) continue;
        if (rest.isEmpty()) return &it;
        if (it.sub) return it.sub->find (rest);
    }
    return nullptr;
}

// ===========================================================================
// Free functions
// ===========================================================================
void showMenu (Component& anchor, Menu menu, MenuOptions options)
{
    if (auto* layer = layerFor (anchor, true))
        layer->open (anchor, std::move (menu), options);
    else
        jassertfalse;   // the anchor isn't inside a MenuHost
}

void showContextMenu (Component& anchor, Point<int> where, Menu menu)
{
    MenuOptions o;
    o.place = MenuOptions::Place::AtPoint;
    o.point = where;
    o.matchAnchorWidth = false;
    showMenu (anchor, std::move (menu), o);
}

void dismissMenus (Component& c)
{
    if (auto* layer = layerFor (c, false))
        layer->dismiss();
}

bool isMenuOpen (Component& c)
{
    auto* layer = layerFor (c, false);
    return layer != nullptr && layer->isOpen();
}

// ===========================================================================
// MenuHost
// ===========================================================================
MenuHost::MenuHost() = default;
MenuHost::~MenuHost() { layer.reset(); }

MenuLayer& MenuHost::getMenuLayer()
{
    if (layer == nullptr)
    {
        layer = std::make_unique<MenuLayer>();
        addChildComponent (*layer);
    }
    resizeMenuLayer();
    return *layer;
}

void MenuHost::resizeMenuLayer()
{
    if (layer != nullptr)
        layer->setBounds (getLocalBounds());
}

// ===========================================================================
// MenuLayer
// ===========================================================================
MenuLayer::MenuLayer()
{
    setInterceptsMouseClicks (true, true);
    setWantsKeyboardFocus (true);
    setAlwaysOnTop (true);
}

MenuLayer::~MenuLayer() { stopTimer(); panes.clear(); }

Rectangle<int> MenuLayer::available() const { return getLocalBounds().reduced (kMargin); }

void MenuLayer::open (Component& a, Menu menu, MenuOptions options)
{
    dismiss();
    if (menu.empty()) return;

    anchor = &a;
    if (auto* p = getParentComponent()) setBounds (p->getLocalBounds());
    toFront (false);
    setVisible (true);

    auto shared = std::make_shared<Menu> (std::move (menu));
    auto pane = std::make_unique<MenuPane> (*this, shared, 0);
    addAndMakeVisible (*pane);

    const auto anchorArea = getLocalArea (&a, a.getLocalBounds());
    Rectangle<int> target = anchorArea;
    if (options.place == MenuOptions::Place::AtPoint)
        target = Rectangle<int> (1, 1).withPosition (getLocalPoint (&a, options.point));

    int minW = jmax (options.minWidth, shared->getMinimumWidth());
    if (options.matchAnchorWidth && (options.place == MenuOptions::Place::Below || options.place == MenuOptions::Place::Above))
        minW = jmax (minW, anchorArea.getWidth());

    pane->place (target, options.place, minW, available());
    panes.push_back (std::move (pane));
    lastMouse = prevMouse = { -10000, -10000 };
    startTimerHz (20);
    if (isShowing()) grabKeyboardFocus();
    repaint();
}

void MenuLayer::dismiss()
{
    stopTimer();
    if (panes.empty() && ! isVisible()) return;
    for (auto& p : panes) p->setVisible (false);
    panes.clear();
    anchor = nullptr;
    setVisible (false);
}

int MenuLayer::depthOf (const MenuPane& p) const
{
    for (size_t i = 0; i < panes.size(); ++i)
        if (panes[i].get() == &p) return (int) i;
    return -1;
}

void MenuLayer::closeAbove (int depth)
{
    while ((int) panes.size() > depth + 1)
        panes.pop_back();
    if (auto* p = pane (depth)) p->setChildRow (-1);
}

void MenuLayer::openSubmenu (MenuPane& parent, int row)
{
    const int depth = depthOf (parent);
    if (depth < 0) return;
    const auto& rows = parent.getRows();
    if (! isPositiveAndBelow (row, (int) rows.size())) return;
    const auto* item = rows[(size_t) row].item;
    if (item == nullptr || item->kind != Menu::Item::Kind::Submenu || ! item->enabled || ! item->sub) return;
    if (parent.openRow() == row && (int) panes.size() > depth + 1) return;

    closeAbove (depth);
    auto child = std::make_unique<MenuPane> (*this, item->sub, depth + 1);
    addAndMakeVisible (*child);
    const auto rowArea = parent.rowBoundsOnScreen (row) + parent.getPosition();
    child->place (rowArea, MenuOptions::Place::Right, 0, available());
    parent.setChildRow (row);
    panes.push_back (std::move (child));
}

bool MenuLayer::headingIntoChild (const MenuPane& parent, Point<int> p) const
{
    const int depth = depthOf (parent);
    if (depth < 0 || (int) panes.size() <= depth + 1) return false;
    const auto child = panes[(size_t) depth + 1]->getBounds();
    // The triangle from where the pointer was to the near edge of the child.
    const bool childRight = child.getX() >= parent.getRight() - 4;
    const float edgeX = (float) (childRight ? child.getX() : child.getRight());
    const auto a = prevMouse.toFloat();
    const Point<float> b (edgeX, (float) child.getY() - 4.0f), c (edgeX, (float) child.getBottom() + 4.0f);
    const auto q = p.toFloat();
    auto sign = [] (Point<float> p1, Point<float> p2, Point<float> p3)
    { return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y); };
    const float d1 = sign (q, a, b), d2 = sign (q, b, c), d3 = sign (q, c, a);
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    // Moving away horizontally means the user isn't heading for it.
    const bool towards = childRight ? q.x >= a.x - 0.5f : q.x <= a.x + 0.5f;
    return towards && ! (neg && pos);
}

void MenuLayer::choose (const Menu::Item& item)
{
    if (! item.enabled || item.kind != Menu::Item::Kind::Action) return;
    if (item.keepOpen) { runKeepOpen (item); return; }

    auto action = item.action;
    lastChosen = item.text;
    Component::SafePointer<Component> owner = anchor;
    dismiss();
    if (action)
        MessageManager::callAsync ([action, owner]
        {
            if (owner != nullptr)
                action();
        });
}

void MenuLayer::runKeepOpen (const Menu::Item& item)
{
    lastChosen = item.text;
    if (item.action) item.action();
    for (auto& p : panes) p->repaint();
}

void MenuLayer::paint (Graphics&) {}

void MenuLayer::mouseDown (const MouseEvent&)
{
    // A click outside every pane: close, and swallow the click.
    dismiss();
}

void MenuLayer::mouseWheelMove (const MouseEvent&, const MouseWheelDetails&) {}

bool MenuLayer::keyPressed (const KeyPress& k)
{
    if (panes.empty()) return false;
    if (panes.back()->key (k)) return true;
    // Nothing used it. Swallow keys anyway while the menu is open, so the
    // panel's own shortcuts don't fire underneath an open menu.
    return true;
}

void MenuLayer::timerCallback()
{
    // The component that opened the menu went away, was hidden, or was
    // moved out of the panel: close quietly.
    auto stillThere = [this]
    {
        auto* host = getParentComponent();
        if (anchor == nullptr || host == nullptr) return false;
        for (auto* c = anchor.getComponent(); c != host; c = c->getParentComponent())
            if (c == nullptr || ! c->isVisible()) return false;
        return true;
    };
    if (! stillThere())
    {
        dismiss();
        return;
    }
}

// ===========================================================================
// MenuPane
// ===========================================================================
MenuPane::MenuPane (MenuLayer& l, std::shared_ptr<Menu> m, int)
    : layer (l), menu (std::move (m))
{
    setWantsKeyboardFocus (false);
    setInterceptsMouseClicks (true, false);
    rebuildRows();
}

int MenuPane::contentTop() const
{
    int top = kBorder;
    if (menu->getTitle().isNotEmpty()) top += kTitleH;
    if (menu->isSearchable()) top += kFilterH;
    return top;
}

Rectangle<int> MenuPane::viewport() const
{
    return getLocalBounds().reduced (kBorder).withTop (contentTop());
}

void MenuPane::rebuildRows()
{
    rows.clear();
    if (filter.isEmpty())
    {
        for (const auto& it : menu->items())
            rows.push_back ({ &it, {}, {} });
    }
    else
    {
        std::vector<Menu::Leaf> leaves;
        menu->collectLeaves (leaves);
        const auto words = StringArray::fromTokens (filter.toLowerCase(), " ", "");
        for (const auto& lf : leaves)
        {
            const auto hay = (lf.path + " " + lf.item->text).toLowerCase();
            bool ok = true;
            for (const auto& w : words)
                if (w.isNotEmpty() && ! hay.contains (w)) { ok = false; break; }
            if (ok)
                rows.push_back ({ lf.item, lf.path, {} });
        }
    }
}

void MenuPane::place (Rectangle<int> target, MenuOptions::Place how, int minW, Rectangle<int> area)
{
    targetArea = target;
    placement = how;
    wantMinWidth = minW;
    fitArea = area;

    // Column width: widest row.
    const auto f = rowFont();
    int w = jmax (kMinColWidth, minW - 2 * kBorder);
    for (const auto& r : rows)
    {
        if (r.item == nullptr) continue;
        int tw = textWidth (r.item->kind == Menu::Item::Kind::Header ? headerFont() : f, r.item->text);
        if (r.item->detail.isNotEmpty()) tw += 8 + textWidth (f, r.item->detail);
        if (r.path.isNotEmpty()) tw += 10 + textWidth (headerFont(), r.path);
        if (r.item->shortcut.isNotEmpty()) tw += 18 + textWidth (headerFont(), r.item->shortcut);
        w = jmax (w, tw + kPadX * 2 + kTickW + kArrowW);
    }
    if (menu->getTitle().isNotEmpty()) w = jmax (w, textWidth (headerFont(), menu->getTitle()) + 2 * kPadX);
    colWidth = jmin (w, kMaxColWidth);
    layoutRows();
}

void MenuPane::layoutRows()
{
    // Split into columns at explicit breaks first.
    std::vector<std::vector<int>> cols (1);
    for (int i = 0; i < (int) rows.size(); ++i)
    {
        if (rows[(size_t) i].item->kind == Menu::Item::Kind::ColumnBreak && filter.isEmpty())
            cols.emplace_back();
        else
            cols.back().push_back (i);
    }
    auto colHeight = [this] (const std::vector<int>& c)
    {
        int h = 0;
        for (int i : c) h += rowHeight (*rows[(size_t) i].item);
        return h;
    };

    const int chrome = contentTop() + kBorder;
    const int maxH = fitArea.getHeight() - chrome;
    int tallest = 0;
    for (auto& c : cols) tallest = jmax (tallest, colHeight (c));

    // Too tall: split evenly into as many columns as fit across the panel.
    if (tallest > maxH && cols.size() == 1 && filter.isEmpty())
    {
        const int total = tallest;
        const int maxCols = jmax (1, (fitArea.getWidth() - 2 * kBorder) / colWidth);
        const int want = jmin (maxCols, (total + maxH - 1) / jmax (1, maxH));
        if (want > 1)
        {
            const int perCol = (total + want - 1) / want;
            std::vector<std::vector<int>> split (1);
            int h = 0;
            for (int i : cols[0])
            {
                const int rh = rowHeight (*rows[(size_t) i].item);
                if (h + rh > perCol && ! split.back().empty() && (int) split.size() < want)
                {
                    split.emplace_back();
                    h = 0;
                }
                // Don't start a column with a separator.
                if (split.back().empty() && rows[(size_t) i].item->kind == Menu::Item::Kind::Separator) continue;
                split.back().push_back (i);
                h += rh;
            }
            cols = split;
            tallest = 0;
            for (auto& c : cols) tallest = jmax (tallest, colHeight (c));
        }
    }

    columns = (int) cols.size();
    for (auto& r : rows) r.bounds = {};
    int x = kBorder;
    for (auto& c : cols)
    {
        int y = 0;
        for (int i : c)
        {
            const int rh = rowHeight (*rows[(size_t) i].item);
            rows[(size_t) i].bounds = { x, y, colWidth, rh };
            y += rh;
        }
        x += colWidth;
    }
    contentHeight = jmax (tallest, filter.isNotEmpty() && rows.empty() ? kRowH : 0);

    const int width = columns * colWidth + 2 * kBorder;
    const int height = jmin (contentHeight, maxH) + chrome;
    maxScroll = jmax (0, contentHeight - (height - chrome));
    scroll = jlimit (0, maxScroll, scroll);

    // Position.
    Rectangle<int> b (width, height);
    const auto& t = targetArea;
    const auto& a = fitArea;
    switch (placement)
    {
        case MenuOptions::Place::Below:
        case MenuOptions::Place::Above:
        {
            const int spaceBelow = a.getBottom() - t.getBottom();
            const int spaceAbove = t.getY() - a.getY();
            const bool below = placement == MenuOptions::Place::Below ? (height <= spaceBelow || spaceBelow >= spaceAbove)
                                                                      : ! (height <= spaceAbove || spaceAbove >= spaceBelow);
            b.setPosition (t.getX(), below ? t.getBottom() : t.getY() - height);
            break;
        }
        case MenuOptions::Place::Right:
        {
            // Sub-menu: line the first row up with the parent row.
            const bool right = t.getRight() + width <= a.getRight() || t.getX() - a.getX() < a.getRight() - t.getRight();
            b.setPosition (right ? t.getRight() - 2 : t.getX() - width + 2, t.getY() - contentTop());
            break;
        }
        case MenuOptions::Place::AtPoint:
        {
            b.setPosition (t.getX() + 2, t.getY() + 2);
            if (b.getBottom() > a.getBottom()) b.setY (t.getY() - height);   // flip up at the bottom edge
            if (b.getRight() > a.getRight())   b.setX (t.getX() - width);    // flip left at the right edge
            break;
        }
    }
    b = b.constrainedWithin (a);
    setBounds (b);

    if (hot < 0)
    {
        // Start on the current value.
        for (int i = 0; i < (int) rows.size(); ++i)
            if (rows[(size_t) i].item->selectable() && rows[(size_t) i].item->isTicked() && rows[(size_t) i].item->kind == Menu::Item::Kind::Action)
            { hot = i; break; }
        if (hot >= 0) ensureVisible (hot);
    }
    repaint();
}

int MenuPane::rowAt (Point<int> p) const
{
    const auto vp = viewport();
    if (! vp.contains (p)) return -1;
    const Point<int> c (p.x, p.y - vp.getY() + scroll);
    for (int i = 0; i < (int) rows.size(); ++i)
        if (rows[(size_t) i].bounds.contains (c)) return i;
    return -1;
}

int MenuPane::findRow (const String& text) const
{
    for (int i = 0; i < (int) rows.size(); ++i)
        if (rows[(size_t) i].item->text == text) return i;
    return -1;
}

Rectangle<int> MenuPane::rowBoundsOnScreen (int row) const
{
    if (! isPositiveAndBelow (row, (int) rows.size())) return {};
    return rows[(size_t) row].bounds.translated (0, viewport().getY() - scroll);
}

void MenuPane::ensureVisible (int row)
{
    if (! isPositiveAndBelow (row, (int) rows.size())) return;
    const auto b = rows[(size_t) row].bounds;
    const int viewH = viewport().getHeight();
    if (b.getY() < scroll) scroll = b.getY();
    else if (b.getBottom() > scroll + viewH) scroll = b.getBottom() - viewH;
    scroll = jlimit (0, maxScroll, scroll);
}

void MenuPane::setHot (int row, bool fromKeyboard)
{
    if (row == hot) return;
    hot = row;
    if (fromKeyboard) ensureVisible (row);
    repaint();
}

int MenuPane::moveHot (int from, int delta) const
{
    const int n = (int) rows.size();
    if (n == 0) return -1;
    int i = from < 0 ? (delta > 0 ? -1 : n) : from;
    for (int step = 0; step < n; ++step)
    {
        i += delta;
        if (i < 0 || i >= n) return from;           // stop at the ends; don't wrap
        if (rows[(size_t) i].item->selectable()) return i;
    }
    return from;
}

void MenuPane::paint (Graphics& g)
{
    const auto r = getLocalBounds();
    // A flat hard shadow, then the body.
    g.setColour (colours::black);
    g.fillRect (r);
    g.setColour (colours::ink.withAlpha (0.9f));
    g.drawRect (r, 1);

    int y = kBorder;
    if (menu->getTitle().isNotEmpty())
    {
        auto t = Rectangle<int> (kBorder, y, getWidth() - 2 * kBorder, kTitleH);
        g.setColour (colours::panel);
        g.fillRect (t);
        drawText (g, menu->getTitle().toUpperCase(), t.toFloat().reduced ((float) kPadX, 0.0f), 11.5f, colours::brand, true, Justification::centredLeft);
        y += kTitleH;
    }
    if (menu->isSearchable())
    {
        auto f = Rectangle<int> (kBorder + 4, y + 3, getWidth() - 2 * kBorder - 8, kFilterH - 6);
        g.setColour (colours::panel);
        g.fillRect (f);
        g.setColour (colours::inkFaint);
        g.drawRect (f, 1);
        // A small magnifier.
        const auto m = f.removeFromLeft (18).toFloat().withSizeKeepingCentre (9.0f, 9.0f);
        g.setColour (colours::inkDim);
        g.drawEllipse (m.withTrimmedRight (2.0f).withTrimmedBottom (2.0f), 1.3f);
        g.drawLine (m.getRight() - 3.0f, m.getBottom() - 3.0f, m.getRight(), m.getBottom(), 1.5f);
        if (filter.isEmpty())
            drawText (g, "Type to search", f.toFloat(), 12.5f, colours::inkFaint, false, Justification::centredLeft);
        else
            drawText (g, filter + "_", f.toFloat(), 13.0f, colours::lcd, true, Justification::centredLeft);
        y += kFilterH;
    }

    const auto vp = viewport();
    {
        Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (vp);
        g.setColour (colours::panel);
        g.fillRect (vp);

        const auto f = rowFont();
        for (int i = 0; i < (int) rows.size(); ++i)
        {
            const auto& row = rows[(size_t) i];
            const auto& it = *row.item;
            auto b = row.bounds.translated (0, vp.getY() - scroll);
            if (! b.intersects (vp) || b.getHeight() == 0) continue;
            const auto bf = b.toFloat();

            switch (it.kind)
            {
                case Menu::Item::Kind::Separator:
                    g.setColour (colours::inkFaint);
                    g.fillRect (bf.withSizeKeepingCentre (bf.getWidth() - 2.0f * kPadX, 1.0f));
                    continue;
                case Menu::Item::Kind::Header:
                    drawText (g, it.text.toUpperCase(), bf.reduced ((float) kPadX, 0.0f).withTrimmedTop (3.0f), 11.0f, colours::brand, true, Justification::centredLeft);
                    continue;
                case Menu::Item::Kind::ColumnBreak:
                    continue;
                case Menu::Item::Kind::Action:
                case Menu::Item::Kind::Submenu:
                    break;
            }

            const bool isHot = i == hot && it.enabled;
            const bool isOpen = i == childRow;
            if (isHot || isOpen)
            {
                g.setColour (isHot ? colours::cream : colours::creamShade.withAlpha (0.35f));
                g.fillRect (bf.reduced (2.0f, 1.0f));
            }
            const Colour text = ! it.enabled ? colours::inkFaint : (isHot ? colours::black : colours::ink);
            auto area = bf.reduced ((float) kPadX, 0.0f);
            auto tickArea = area.removeFromLeft ((float) kTickW);
            if (it.isTicked())
                drawTick (g, tickArea, isHot ? colours::black : colours::lcd);
            if (it.kind == Menu::Item::Kind::Submenu)
                drawArrow (g, area.removeFromRight ((float) kArrowW), text);
            else
                area.removeFromRight ((float) kArrowW * 0.5f);

            if (! it.swatch.isTransparent())
            {
                g.setColour (it.swatch);
                g.fillRect (area.removeFromLeft (10.0f).withSizeKeepingCentre (8.0f, 8.0f));
                area.removeFromLeft (5.0f);
            }
            if (it.shortcut.isNotEmpty())
            {
                const float sw = (float) textWidth (headerFont(), it.shortcut) + 4.0f;
                drawText (g, it.shortcut, area.removeFromRight (sw), 11.0f, isHot ? colours::black.withAlpha (0.6f) : colours::inkDim, false, Justification::centredRight);
                area.removeFromRight (10.0f);
            }
            if (row.path.isNotEmpty())
            {
                const float pw = jmin (area.getWidth() * 0.5f, (float) textWidth (headerFont(), row.path) + 10.0f);
                drawText (g, row.path, area.removeFromRight (pw), 11.0f, isHot ? colours::black.withAlpha (0.55f) : colours::inkDim, false, Justification::centredRight);
            }
            g.setColour (text);
            g.setFont (f);
            if (it.detail.isNotEmpty())
            {
                const float tw = jmin (area.getWidth(), (float) textWidth (f, it.text) + 8.0f);
                g.drawText (it.text, area.removeFromLeft (tw), Justification::centredLeft, true);
                drawText (g, it.detail, area, 12.0f, isHot ? colours::black.withAlpha (0.55f) : colours::inkDim, false, Justification::centredLeft);
            }
            else
            {
                g.drawText (it.text, area, Justification::centredLeft, true);
            }
        }

        if (rows.empty() && filter.isNotEmpty())
            drawText (g, "No matches", vp.toFloat().removeFromTop ((float) kRowH), 13.0f, colours::inkDim, false);

        // Column rules.
        g.setColour (colours::inkFaint);
        for (int c = 1; c < columns; ++c)
            g.fillRect (Rectangle<int> (kBorder + c * colWidth, vp.getY() + 4, 1, vp.getHeight() - 8));
    }

    // More above / below.
    if (scroll > 0)
    {
        auto top = vp.withHeight (12).toFloat();
        g.setColour (colours::panel);
        g.fillRect (top);
        drawArrow (g, top, colours::ink, false);
        // (an up arrow: flip the down arrow)
    }
    if (scroll < maxScroll)
    {
        auto bot = vp.withTop (vp.getBottom() - 12).toFloat();
        g.setColour (colours::panel);
        g.fillRect (bot);
        drawArrow (g, bot, colours::ink, false);
    }
}

void MenuPane::mouseMove (const MouseEvent& e)
{
    const auto p = e.getPosition();
    const auto inLayer = p + getPosition();
    layer.prevMouse = layer.lastMouse;
    layer.lastMouse = inLayer;

    int r = rowAt (p);
    if (r >= 0 && ! rows[(size_t) r].item->selectable()) r = -1;
    if (r == hot) return;

    // Heading for the open sub-menu across other rows: hold still.
    if (childRow >= 0 && r != childRow && layer.headingIntoChild (*this, inLayer))
    {
        pendingRow = r;
        pendingOpen = false;
        startTimer (kSwitchDelayMs);
        return;
    }

    setHot (r, false);
    if (r >= 0 && rows[(size_t) r].item->kind == Menu::Item::Kind::Submenu)
    {
        pendingRow = r;
        pendingOpen = true;
        startTimer (childRow >= 0 ? kSwitchDelayMs / 2 : kOpenDelayMs);
    }
    else if (childRow >= 0)
    {
        pendingRow = r;
        pendingOpen = false;
        startTimer (kSwitchDelayMs);
    }
    else
    {
        stopTimer();
    }
}

void MenuPane::timerCallback()
{
    stopTimer();
    const int d = layer.depthOf (*this);
    if (d < 0) return;
    // Hovered row may have changed while waiting.
    const int now = rowAt (layer.lastMouse - getPosition());
    if (pendingOpen)
    {
        if (now == pendingRow) layer.openSubmenu (*this, pendingRow);
        return;
    }
    // Close (or switch) the open sub-menu unless the pointer made it there.
    if (layer.pane (d + 1) != nullptr && layer.pane (d + 1)->getBounds().contains (layer.lastMouse))
        return;
    if (now != childRow)
    {
        layer.closeAbove (d);
        setHot (now >= 0 && rows[(size_t) now].item->selectable() ? now : -1, false);
        if (hot >= 0 && rows[(size_t) hot].item->kind == Menu::Item::Kind::Submenu)
            layer.openSubmenu (*this, hot);
    }
}

void MenuPane::mouseExit (const MouseEvent&)
{
    // Keep the highlight on an open sub-menu's row; otherwise clear it.
    if (childRow < 0 && hot >= 0) setHot (-1, false);
}

void MenuPane::mouseDown (const MouseEvent& e)
{
    pressedRow = rowAt (e.getPosition());
    if (pressedRow >= 0 && rows[(size_t) pressedRow].item->kind == Menu::Item::Kind::Submenu)
    {
        stopTimer();
        setHot (pressedRow, false);
        layer.openSubmenu (*this, pressedRow);
    }
}

void MenuPane::mouseUp (const MouseEvent& e)
{
    const int r = rowAt (e.getPosition());
    const int down = pressedRow;
    pressedRow = -1;
    if (r < 0 || r != down) return;   // never fire on a release that didn't start here
    const auto* it = rows[(size_t) r].item;
    if (it->kind == Menu::Item::Kind::Action && it->enabled)
        layer.choose (*it);           // may delete this pane
}

void MenuPane::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
{
    if (maxScroll <= 0) return;
    scroll = jlimit (0, maxScroll, scroll - roundToInt (w.deltaY * 220.0f));
    layer.closeAbove (layer.depthOf (*this));
    repaint();
}

bool MenuPane::key (const KeyPress& k)
{
    const int code = k.getKeyCode();
    const int d = layer.depthOf (*this);

    if (code == KeyPress::escapeKey)
    {
        if (filter.isNotEmpty()) { filter.clear(); hot = -1; rebuildRows(); place (targetArea, placement, wantMinWidth, fitArea); return true; }
        if (d > 0) { layer.closeAbove (d - 1); return true; }
        layer.dismiss();
        return true;
    }
    if (code == KeyPress::downKey || code == KeyPress::upKey)
    {
        setHot (moveHot (hot, code == KeyPress::downKey ? 1 : -1), true);
        return true;
    }
    if (code == KeyPress::homeKey) { setHot (moveHot (-1, 1), true); return true; }
    if (code == KeyPress::endKey)  { setHot (moveHot ((int) rows.size(), -1), true); return true; }
    if (code == KeyPress::pageDownKey || code == KeyPress::pageUpKey)
    {
        int h = hot;
        for (int i = 0; i < 10; ++i) h = moveHot (h, code == KeyPress::pageDownKey ? 1 : -1);
        setHot (h, true);
        return true;
    }
    if (code == KeyPress::rightKey)
    {
        if (columns > 1 && hot >= 0 && rows[(size_t) hot].item->kind != Menu::Item::Kind::Submenu)
        {
            // Jump to the row beside this one in the next column.
            const auto b = rows[(size_t) hot].bounds;
            for (int i = 0; i < (int) rows.size(); ++i)
                if (rows[(size_t) i].item->selectable() && rows[(size_t) i].bounds.getX() == b.getX() + colWidth
                    && rows[(size_t) i].bounds.getBottom() > b.getY())
                { setHot (i, true); return true; }
            return true;
        }
        if (hot >= 0 && rows[(size_t) hot].item->kind == Menu::Item::Kind::Submenu)
        {
            layer.openSubmenu (*this, hot);
            if (auto* child = layer.pane (d + 1)) child->setHot (child->moveHot (-1, 1), true);
        }
        return true;
    }
    if (code == KeyPress::leftKey)
    {
        if (columns > 1 && hot >= 0)
        {
            const auto b = rows[(size_t) hot].bounds;
            for (int i = 0; i < (int) rows.size(); ++i)
                if (rows[(size_t) i].item->selectable() && rows[(size_t) i].bounds.getX() == b.getX() - colWidth
                    && rows[(size_t) i].bounds.getBottom() > b.getY())
                { setHot (i, true); return true; }
        }
        if (d > 0) layer.closeAbove (d - 1);
        return true;
    }
    if (code == KeyPress::returnKey || (code == KeyPress::spaceKey && filter.isEmpty()))
    {
        if (hot < 0) return true;
        const auto* it = rows[(size_t) hot].item;
        if (it->kind == Menu::Item::Kind::Submenu)
        {
            layer.openSubmenu (*this, hot);
            if (auto* child = layer.pane (d + 1)) child->setHot (child->moveHot (-1, 1), true);
        }
        else
            layer.choose (*it);    // may delete this pane
        return true;
    }

    const auto ch = k.getTextCharacter();
    if (menu->isSearchable())
    {
        if (code == KeyPress::backspaceKey)
        {
            if (filter.isNotEmpty()) { filter = filter.dropLastCharacters (1); hot = -1; rebuildRows(); place (targetArea, placement, wantMinWidth, fitArea); setHot (moveHot (-1, 1), true); }
            return true;
        }
        if (ch >= ' ' && ! k.getModifiers().isCommandDown())
        {
            layer.closeAbove (d);
            filter << ch;
            hot = -1;
            rebuildRows();
            place (targetArea, placement, wantMinWidth, fitArea);
            setHot (moveHot (-1, 1), true);
            return true;
        }
        return false;
    }
    if (CharacterFunctions::isLetterOrDigit (ch))
    {
        // Jump to the next item starting with this character.
        const auto lower = CharacterFunctions::toLowerCase (ch);
        const int n = (int) rows.size();
        for (int step = 1; step <= n; ++step)
        {
            const int i = (jmax (hot, -1) + step + n) % n;
            const auto& it = *rows[(size_t) i].item;
            if (it.selectable() && it.text.isNotEmpty() && CharacterFunctions::toLowerCase (it.text[0]) == lower)
            { setHot (i, true); return true; }
        }
        return true;
    }
    return false;
}
} // namespace tincture::ui
