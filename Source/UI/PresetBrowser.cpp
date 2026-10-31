// PresetBrowser.cpp — the preset browser overlay (spec E §8): folder tree,
// CATEGORIES / TAGS chips with a rating filter, search, the preset table
// (NAME / LOCATION / CATEGORY / DATE MODIFIED / RATING), the ⚙ presets menu,
// row right-click menu, and the metadata pane (category, description, tags).
#include "FxModules.h"
#include "../Model/FactoryPresets.h"

namespace tincture::ui
{
using namespace juce;

namespace
{
const String kStar = String (CharPointer_UTF8 ("\xe2\x98\x85"));
String stars (int n) { String s; for (int i = 0; i < n; ++i) s += kStar; return s; }

const StringArray& allCategories()
{
    static const StringArray c { "808", "Acid", "Ambience", "Analog", "Arpeggio", "Audio Effect", "Bass", "Bell", "Brass", "Choir", "Chord",
                                 "Downlift", "Drum", "Drumkit", "Guitar", "HiHat", "Hit/Stab", "Hoover", "Instrument", "Keyboard", "Lead",
                                 "Loop", "Midrange", "Organ", "Pad", "Perc", "Pluck", "Sawtooth", "Seq", "SFX", "String", "Synth", "Template" };
    return c;
}

// Rewrites a user preset's metadata in place.
bool modifyInfo (const File& f, const std::function<void (Node&)>& fn)
{
    Node root;
    if (! fxui::readNode (f, root)) return false;
    bool done = false;
    for (auto& c : root.allChildren())
        if (c.type() == "patch")
            for (auto& inf : c.allChildren())
                if (inf.type() == "info") { fn (inf); done = true; }
    return done && fxui::writeNode (f, root);
}

class Browser;

// ---------------------------------------------------------------------------
class PresetTable final : public Component
{
public:
    explicit PresetTable (Browser& b) : br (b) {}
    void paint (Graphics&) override;
    void mouseDown (const MouseEvent&) override;
    void mouseDoubleClick (const MouseEvent&) override;
    static constexpr int rowH = 26;
    Browser& br;
};

class Browser final : public Component, public Refreshable, public TextEditor::Listener
{
public:
    Browser (PanelContext& c, std::function<void()> close) : ctx (c), proc (c.proc), table (*this), onClose (std::move (close))
    {
        ctx.add (this);
        setWantsKeyboardFocus (true);
        search.setTextToShowWhenEmpty ("Search presets", colours::inkDim);
        search.setFont (panelFont (14.0f, false));
        search.addListener (this);
        search.setJustification (Justification::centredLeft);
        search.setIndents (8, 0);
        search.setColour (TextEditor::backgroundColourId, colours::black);
        search.setColour (TextEditor::textColourId, colours::lcd);
        search.setColour (TextEditor::outlineColourId, colours::inkDim);
        addAndMakeVisible (search);
        clearSearch.onClick = [this] (const ModifierKeys&) { search.clear(); applyFilter(); };
        addAndMakeVisible (clearSearch);
        cog.onClick = [this] (const ModifierKeys&) { showMenu (cog, presetsMenu(), MenuOptions { MenuOptions::Place::Below, {}, 300, false }); };
        cog.setTooltip ("Presets menu");
        addAndMakeVisible (cog);
        closeBtn.onClick = [this] (const ModifierKeys&) { if (onClose) onClose(); };
        closeBtn.setTooltip ("Close the browser");
        addAndMakeVisible (closeBtn);
        catTab.onClick = [this] (const ModifierKeys&) { showTags = false; layoutChips(); };
        tagTab.onClick = [this] (const ModifierKeys&) { showTags = true; layoutChips(); };
        addAndMakeVisible (catTab);
        addAndMakeVisible (tagTab);
        rating.getText = [this] { return ratingFilter < 0 ? String ("(any rating)") : ratingFilter == 0 ? String ("(no rating)") : stars (ratingFilter); };
        rating.onOpen = [this]
        {
            Menu m;
            m.add ("(any rating)", [this] { ratingFilter = -1; applyFilter(); }, ratingFilter == -1);
            m.add ("(no rating)", [this] { ratingFilter = 0; applyFilter(); }, ratingFilter == 0);
            for (int i = 1; i <= 5; ++i) m.add (stars (i), [this, i] { ratingFilter = i; applyFilter(); }, ratingFilter == i);
            showMenu (rating, m);
        };
        rating.fontH = 12.0f;
        addAndMakeVisible (rating);
        catBox.getText = [this] { auto* e = selectedEntry(); return e == nullptr || e->category.isEmpty() ? String ("(no category)") : e->category; };
        catBox.onOpen = [this] { categoryMenu(); };
        catBox.fontH = 12.5f;
        addAndMakeVisible (catBox);
        descBtn.onClick = [this] (const ModifierKeys&) { editDescription(); };
        descBtn.setTooltip ("Edit the description (user presets)");
        addAndMakeVisible (descBtn);
        addTag.onClick = [this] (const ModifierKeys&) { tagMenu(); };
        addTag.setTooltip ("Add a tag (user presets)");
        addAndMakeVisible (addTag);
        view.setViewedComponent (&table, false);
        view.setScrollBarsShown (true, false);
        view.setScrollBarThickness (10);
        addAndMakeVisible (view);
        rescan();
        select (-1);
    }
    ~Browser() override { ctx.remove (this); }

    // --- data -----------------------------------------------------------------
    void rescan()
    {
        proc.presets.rescan();
        buildFolders();
        buildChips();
        applyFilter();
    }
    const std::vector<PresetEntry>& entries() { return proc.presets.all(); }
    PresetEntry* selectedEntry()
    {
        auto& all = const_cast<std::vector<PresetEntry>&> (proc.presets.all());
        return selected >= 0 && selected < (int) all.size() ? &all[(size_t) selected] : nullptr;
    }
    String location (const PresetEntry& e) const { return e.factory ? "Factory/" + e.category : (e.folder.isEmpty() ? String ("User") : "User/" + e.folder); }
    String dateOf (const PresetEntry& e) const { return e.factory ? String ("-") : e.file.getLastModificationTime().formatted ("%Y-%m-%d"); }

    void buildFolders()
    {
        folders.clear();
        folders.push_back ({ "Local", "", 0 });
        folders.push_back ({ "Factory", "F", 1 });
        StringArray cats, dirs;
        for (auto& e : entries())
        {
            if (e.factory) cats.addIfNotAlreadyThere (e.category.isEmpty() ? "Other" : e.category);
            else if (e.folder.isNotEmpty()) dirs.addIfNotAlreadyThere (e.folder.replaceCharacter ('\\', '/'));
        }
        cats.sort (true);
        dirs.sort (true);
        if (expanded.count ("F")) for (auto& c : cats) folders.push_back ({ c, "F/" + c, 2 });
        folders.push_back ({ "User", "U", 1 });
        if (expanded.count ("U")) for (auto& d : dirs) folders.push_back ({ d.fromLastOccurrenceOf ("/", false, false), "U/" + d, 2 + d.retainCharacters ("/").length() });
    }
    void buildChips()
    {
        StringArray cats { "(no category)" }, tags;
        for (auto& e : entries())
        {
            if (e.category.isNotEmpty()) cats.addIfNotAlreadyThere (e.category);
            for (auto& t : e.tags) tags.addIfNotAlreadyThere (t);
        }
        cats.sort (true);
        cats.move (cats.indexOf ("(no category)"), 0);
        tags.sort (true);
        catChips = cats;
        tagChips = tags;
        layoutChips();
    }
    bool matches (const PresetEntry& e) const
    {
        if (folderKey == "F" && ! e.factory) return false;
        if (folderKey == "U" && e.factory) return false;
        if (folderKey.startsWith ("F/") && (! e.factory || (e.category.isEmpty() ? String ("Other") : e.category) != folderKey.substring (2))) return false;
        if (folderKey.startsWith ("U/") && (e.factory || ! (e.folder.replaceCharacter ('\\', '/') + "/").startsWith (folderKey.substring (2) + "/"))) return false;
        const auto q = search.getText().trim();
        if (q.isNotEmpty())
        {
            for (auto& w : StringArray::fromTokens (q, " ", ""))
                if (! (e.name.containsIgnoreCase (w) || e.category.containsIgnoreCase (w) || e.author.containsIgnoreCase (w)
                       || e.description.containsIgnoreCase (w) || e.tags.joinIntoString (",").containsIgnoreCase (w)))
                    return false;
        }
        if (ratingFilter >= 0 && e.rating != ratingFilter) return false;
        if (! selCats.isEmpty())
            if (! selCats.contains (e.category.isEmpty() ? String ("(no category)") : e.category)) return false;
        for (auto& t : selTags) if (! e.tags.contains (t)) return false;
        return true;
    }
    void applyFilter()
    {
        visible.clear();
        const auto& all = entries();
        for (int i = 0; i < (int) all.size(); ++i) if (matches (all[(size_t) i])) visible.push_back (i);
        std::sort (visible.begin(), visible.end(), [this, &all] (int a, int b)
        {
            const auto& x = all[(size_t) a];
            const auto& y = all[(size_t) b];
            int c = 0;
            switch (sortCol)
            {
                case 1: c = location (x).compareIgnoreCase (location (y)); break;
                case 2: c = x.category.compareIgnoreCase (y.category); break;
                case 3: c = dateOf (x).compare (dateOf (y)); break;
                case 4: c = x.rating - y.rating; break;
                default: break;
            }
            if (c == 0) c = x.name.compareIgnoreCase (y.name);
            return sortAsc ? c < 0 : c > 0;
        });
        table.setSize (view.getWidth() - 12, jmax (view.getHeight(), (int) visible.size() * PresetTable::rowH));
        table.repaint();
        rating.refresh();
        repaint();
    }
    void textEditorTextChanged (TextEditor&) override { applyFilter(); }
    void textEditorReturnKeyPressed (TextEditor&) override { applyFilter(); }
    void textEditorEscapeKeyPressed (TextEditor&) override { search.clear(); applyFilter(); }

    // --- actions --------------------------------------------------------------
    void load (int i)
    {
        const auto& all = entries();
        if (i < 0 || i >= (int) all.size()) return;
        const auto e = all[(size_t) i];
        select (i);
        if (e.factory)
        {
            Patch p;
            ParamValues v;
            v.resetToDefaults();
            factoryPresets()[(size_t) e.factoryIndex].build (p, v);
            p.info.name = e.name.toStdString();
            p.info.category = e.category.toStdString();
            p.info.author = "Tincture";
            p.info.description = e.description.toStdString();
            const auto bin = encodePreset (p, v).toBinary();
            proc.loadPresetData (MemoryBlock (bin.data(), bin.size()), e.name);
        }
        else proc.loadPreset (e.file);
        loaded = i;
        table.repaint();
        repaint();
    }
    void select (int i)
    {
        selected = i;
        const auto* e = selectedEntry();
        const bool user = e != nullptr && ! e->factory;
        for (auto* b : { &descBtn, &addTag }) { b->setEnabled (user); b->setDimmed (! user); }
        catBox.refresh();
        table.repaint();
        repaint();
    }
    void setRating (int i, int r)
    {
        const auto& all = entries();
        if (i < 0 || i >= (int) all.size()) return;
        proc.presets.setRating (i, all[(size_t) i].rating == r && r == 1 ? 0 : r);
        if (ratingFilter >= 0) applyFilter(); else table.repaint();
    }
    void loadRandom()
    {
        if (visible.empty()) return;
        load (visible[(size_t) Random::getSystemRandom().nextInt ((int) visible.size())]);
    }
    void deleteSelected()
    {
        auto* e = selectedEntry();
        if (e == nullptr || e->factory) return;
        e->file.moveToTrash();
        selected = loaded = -1;
        rescan();
    }
    void renameSelected (Component& anchor)
    {
        auto* e = selectedEntry();
        if (e == nullptr || e->factory || ! ctx.editValue) return;
        const auto file = e->file;
        const auto rel = file.getRelativePathFrom (Settings::userPresets()).upToLastOccurrenceOf (PresetStore::extension, false, true);
        ctx.editValue (anchor, rel, [this, file] (const String& t)
        {
            // "Folder/Name" moves into a sub-folder of the user presets.
            auto path = t.trim().replaceCharacter ('\\', '/');
            if (path.isEmpty()) return;
            auto dest = Settings::userPresets();
            for (auto& part : StringArray::fromTokens (path, "/", ""))
                if (part.isNotEmpty()) dest = dest.getChildFile (File::createLegalFileName (part));
            dest = dest.withFileExtension (PresetStore::extension);
            if (dest == file || dest.exists()) return;
            dest.getParentDirectory().createDirectory();
            if (file.moveFileTo (dest)) rescan();
        });
    }
    void editUserInfo (const String&, const std::function<void (Node&)>& fn)
    {
        auto* e = selectedEntry();
        if (e == nullptr || e->factory) return;
        const auto f = e->file;
        modifyInfo (f, fn);
        const int keep = selected;
        const auto keepFile = f;
        proc.presets.rescan();
        selected = proc.presets.indexOfFile (keepFile);
        if (selected < 0) selected = keep;
        buildChips();
        applyFilter();
        catBox.refresh();
    }
    void categoryMenu()
    {
        auto* e = selectedEntry();
        const bool user = e != nullptr && ! e->factory;
        const auto cur = e != nullptr ? e->category : String();
        Menu m;
        auto setCat = [this] (const String& c) { editUserInfo ("Category", [c] (Node& n) { n.set ("category", c.toStdString()); }); };
        m.add ("(no category)", [setCat] { setCat ({}); }, cur.isEmpty(), user);
        m.add ("Custom...", [this, setCat]
        {
            if (ctx.editValue) ctx.editValue (catBox, {}, [setCat] (const String& t) { if (t.trim().isNotEmpty()) setCat (t.trim()); });
        }, false, user);
        StringArray cats = allCategories();
        for (auto& c : proc.presets.categories()) cats.addIfNotAlreadyThere (c);
        for (auto& c : cats) m.add (c, [setCat, c] { setCat (c); }, c == cur, user);
        showMenu (catBox, m);
    }
    void tagMenu()
    {
        auto* e = selectedEntry();
        if (e == nullptr || e->factory) return;
        StringArray tags { "Arp", "Clip", "Custom-Tuning", "Embedded-Data", "Granular", "KB-Span", "Multisample", "Poly", "S1", "Sample", "SerumFX", "Spectral" };
        for (auto& t : tagChips) tags.addIfNotAlreadyThere (t);
        tags.sort (true);
        const auto have = e->tags;
        Menu m;
        auto addT = [this, have] (const String& t)
        {
            auto all = have;
            all.addIfNotAlreadyThere (t);
            editUserInfo ("Tags", [all] (Node& n) { n.set ("tags", all.joinIntoString (",").toStdString()); });
        };
        for (auto& t : tags) if (! have.contains (t)) m.add (t, [addT, t] { addT (t); });
        m.add ("New tag...", [this, addT] { if (ctx.editValue) ctx.editValue (addTag, {}, [addT] (const String& t) { if (t.trim().isNotEmpty()) addT (t.trim()); }); });
        showMenu (addTag, m);
    }
    void removeTag (const String& t)
    {
        auto* e = selectedEntry();
        if (e == nullptr || e->factory) return;
        auto all = e->tags;
        all.removeString (t);
        editUserInfo ("Tags", [all] (Node& n) { n.set ("tags", all.joinIntoString (",").toStdString()); });
    }
    void editDescription()
    {
        auto* e = selectedEntry();
        if (e == nullptr || e->factory || ! ctx.editValue) return;
        ctx.editValue (descBtn, e->description, [this] (const String& t) { editUserInfo ("Description", [t] (Node& n) { n.set ("description", t.toStdString()); }); });
    }

    Menu presetsMenu()
    {
        auto* e = selectedEntry();
        const bool user = e != nullptr && ! e->factory;
        Menu m;
        m.add ("Show Preset in Finder", [this] { if (auto* s = selectedEntry()) if (! s->factory) s->file.revealToUser(); }, false, user);
        m.add ("Load Random Preset", [this] { loadRandom(); }, false, ! visible.empty()).withShortcut ("7");
        m.add ("Hybridize", {}, false, false).withShortcut ("8");
        m.add ("Hybridize Favoring Selected Preset", {}, false, false).withShortcut ("9");
        m.add ("Delete Preset(s)", [this] { deleteSelected(); }, false, user).withShortcut ("Shift+Bksp");
        m.add ("Rename / Move Preset", [this] { renameSelected (search); }, false, user);
        m.separator();
        m.toggle ("Auto-Play Previews", false, {}, false);
        m.sub ("Preview Fallback Clip", Menu().add ("Chords", {}, true, false).add ("Melody", {}, false, false).add ("Bass", {}, false, false), false, false);
        m.separator();
        m.add ("Rescan Database", [this] { rescan(); });
        m.add ("Erase and Rebuild Database", [this] { selected = loaded = -1; rescan(); });
        return m;
    }
    Menu rowMenu (int i)
    {
        select (i);
        const bool user = selectedEntry() != nullptr && ! selectedEntry()->factory;
        Menu m;
        m.add ("Show in Finder", [this] { if (auto* s = selectedEntry()) s->file.revealToUser(); }, false, user);
        m.add ("Delete", [this] { deleteSelected(); }, false, user);
        m.add ("Rename / Move", [this] { renameSelected (search); }, false, user);
        return m;
    }

    bool keyPressed (const KeyPress& k) override
    {
        if (k.getTextCharacter() == '7') { loadRandom(); return true; }
        if (k == KeyPress::upKey || k == KeyPress::downKey)
        {
            auto it = std::find (visible.begin(), visible.end(), selected);
            int pos = it == visible.end() ? -1 : (int) (it - visible.begin());
            pos = jlimit (0, (int) visible.size() - 1, pos + (k == KeyPress::upKey ? -1 : 1));
            if (! visible.empty()) select (visible[(size_t) pos]);
            return true;
        }
        if (k == KeyPress::returnKey && selected >= 0) { load (selected); return true; }
        if (k.getModifiers().isShiftDown() && k.getKeyCode() == KeyPress::backspaceKey) { deleteSelected(); return true; }
        if (k.getModifiers().isCommandDown() && k.getKeyCode() == 'F') { search.grabKeyboardFocus(); return true; }
        if (k == KeyPress::escapeKey && onClose) { onClose(); return true; }
        return false;
    }

    void refresh() override
    {
        if (! fxui::panelVisible (*this)) return;
        if (proc.presetName() != shownPreset) { shownPreset = proc.presetName(); table.repaint(); }
    }

    // --- layout / paint -------------------------------------------------------
    struct Folder { String label, key; int depth; };
    struct Chip { String text; Rectangle<int> r; };

    Rectangle<int> folderArea() const { return { 12, 30, 282, 360 }; }
    Rectangle<int> chipArea() const { return { 12, 452, 282, 334 }; }
    Rectangle<int> metaArea() const { return { 1062, 64, 326, 722 }; }

    void layoutChips()
    {
        chips.clear();
        const auto& list = showTags ? tagChips : catChips;
        const auto a = chipArea().reduced (8, 8);
        int x = a.getX(), y = a.getY();
        const auto f = panelFont (11.5f, true);
        for (auto& t : list)
        {
            const int w = (int) GlyphArrangement::getStringWidth (f, t) + 16;
            if (x + w > a.getRight()) { x = a.getX(); y += 26; }
            chips.push_back ({ t, { x, y, w, 22 } });
            x += w + 5;
        }
        catTab.setLit (! showTags);
        tagTab.setLit (showTags);
        repaint();
    }

    void resized() override
    {
        catTab.setBounds (12, 408, 100, 26);
        tagTab.setBounds (114, 408, 70, 26);
        rating.setBounds (188, 408, 106, 26);
        search.setBounds (306, 14, 640, 30);
        clearSearch.setBounds (950, 14, 30, 30);
        cog.setBounds (986, 14, 64, 30);
        closeBtn.setBounds (1310, 14, 78, 30);
        view.setBounds (306, 84, 744, 702);
        catBox.setBounds (metaArea().getX() + 14, metaArea().getY() + 156, metaArea().getWidth() - 28, 26);
        descBtn.setBounds (metaArea().getRight() - 66, metaArea().getY() + 196, 52, 20);
        addTag.setBounds (metaArea().getRight() - 66, metaArea().getY() + 396, 52, 20);
        layoutChips();
        applyFilter();
    }

    static constexpr int tableX[6] { 8, 260, 420, 540, 640, 744 };   // NAME, LOCATION, CATEGORY, DATE, RATING, end

    void paint (Graphics& g) override
    {
        g.fillAll (colours::panel);
        drawSection (g, folderArea().toFloat(), "FOLDERS");
        drawSection (g, chipArea().toFloat().withTop (444), {});
        drawSection (g, metaArea().toFloat(), "PRESET");
        // Folders.
        const auto fa = folderArea().reduced (10, 12);
        for (size_t i = 0; i < folders.size(); ++i)
        {
            const auto& f = folders[i];
            const auto r = Rectangle<float> ((float) fa.getX(), (float) fa.getY() + (float) i * 22.0f, (float) fa.getWidth(), 22.0f);
            if (r.getBottom() > (float) fa.getBottom()) break;
            if (f.key == folderKey) { g.setColour (colours::cream); g.fillRect (r); }
            const float x = r.getX() + (float) f.depth * 14.0f;
            if (f.key == "F" || f.key == "U")
            {
                Path tri;
                if (expanded.count (f.key)) tri.addTriangle (x, r.getCentreY() - 3, x + 8, r.getCentreY() - 3, x + 4, r.getCentreY() + 3);
                else tri.addTriangle (x + 1, r.getCentreY() - 4, x + 1, r.getCentreY() + 4, x + 7, r.getCentreY());
                g.setColour (f.key == folderKey ? colours::black : colours::inkDim);
                g.fillPath (tri);
            }
            drawText (g, f.label, r.withLeft (x + 12), 12.5f, f.key == folderKey ? colours::black : colours::ink, f.depth < 2, Justification::centredLeft);
        }
        // Chips.
        const auto& sel = showTags ? selTags : selCats;
        for (auto& c : chips)
        {
            const bool on = sel.contains (c.text);
            g.setColour (on ? colours::cream : colours::black);
            g.fillRoundedRectangle (c.r.toFloat(), 4.0f);
            g.setColour (colours::inkDim);
            g.drawRoundedRectangle (c.r.toFloat(), 4.0f, 1.0f);
            drawText (g, c.text, c.r.toFloat(), 11.5f, on ? colours::black : colours::ink, true);
        }
        // Table header.
        const auto v = view.getBounds();
        g.setColour (colours::black);
        g.fillRect (v.getX(), 52, v.getWidth(), 28);
        const char* names[] { "NAME", "LOCATION", "CATEGORY", "DATE MODIFIED", "RATING" };
        for (int c = 0; c < 5; ++c)
        {
            String t = names[c];
            if (c == sortCol) t << (sortAsc ? "  v" : "  ^");
            drawText (g, t, { (float) (v.getX() + tableX[c]), 52.0f, (float) (tableX[c + 1] - tableX[c]), 28.0f }, 11.5f,
                      c == sortCol ? colours::lcd : colours::ink, true, Justification::centredLeft);
        }
        drawText (g, String ((int) visible.size()) + " presets", { (float) v.getX(), (float) v.getBottom() + 0, 200, 14 }, 10.5f, colours::inkDim, false, Justification::centredLeft);
        // Metadata.
        const auto m = metaArea().reduced (14, 18).toFloat();
        auto* e = selectedEntry();
        auto label = [&] (const String& t, float y) { drawText (g, t, { m.getX(), m.getY() + y, m.getWidth(), 14 }, 11.0f, colours::inkDim, true, Justification::centredLeft); };
        label ("PRESET NAME", 0);
        label ("AUTHOR", 54);
        label ("CATEGORY", 110);
        label ("DESCRIPTION", 182);
        label ("TAGS", 382);
        label ("DATE", 640);
        if (e != nullptr)
        {
            drawText (g, e->name, { m.getX(), m.getY() + 16, m.getWidth(), 24 }, 17.0f, colours::lcd, true, Justification::centredLeft);
            drawText (g, e->author.isEmpty() ? String ("-") : e->author, { m.getX(), m.getY() + 70, m.getWidth(), 20 }, 13.0f, colours::ink, false, Justification::centredLeft);
            g.setColour (colours::ink);
            g.setFont (panelFont (12.5f, false));
            g.drawFittedText (e->description.isEmpty() ? String ("-") : e->description, Rectangle<float> (m.getX(), m.getY() + 202, m.getWidth(), 170).toNearestInt(),
                              Justification::topLeft, 10);
            float x = m.getX(), y = m.getY() + 402;
            tagRects.clear();
            for (auto& t : e->tags)
            {
                const float w = GlyphArrangement::getStringWidth (panelFont (11.5f), t) + (e->factory ? 14.0f : 30.0f);
                if (x + w > m.getRight()) { x = m.getX(); y += 26; }
                const auto r = Rectangle<float> (x, y, w, 22);
                g.setColour (colours::black);
                g.fillRoundedRectangle (r, 4.0f);
                g.setColour (colours::inkDim);
                g.drawRoundedRectangle (r, 4.0f, 1.0f);
                drawText (g, t, r.withTrimmedRight (e->factory ? 0.0f : 16.0f), 11.5f, colours::ink, true);
                if (! e->factory)
                {
                    drawText (g, "x", r.withLeft (r.getRight() - 18), 11.5f, colours::inkDim, true);
                    tagRects.push_back ({ t, r.withLeft (r.getRight() - 18).toNearestInt() });
                }
                x += w + 5;
            }
            drawText (g, dateOf (*e), { m.getX(), m.getY() + 656, m.getWidth(), 20 }, 13.0f, colours::ink, false, Justification::centredLeft);
            if (e->factory)
                drawText (g, "Factory presets are read-only", { m.getX(), m.getY() + 90, m.getWidth(), 16 }, 11.0f, colours::inkDim, false, Justification::centredLeft);
        }
        else drawText (g, "Select a preset", { m.getX(), m.getY() + 16, m.getWidth(), 24 }, 14.0f, colours::inkDim, false, Justification::centredLeft);
    }

    void mouseDown (const MouseEvent& e) override
    {
        grabKeyboardFocus();
        const auto fa = folderArea().reduced (10, 12);
        if (fa.contains (e.getPosition()))
        {
            const int i = (e.y - fa.getY()) / 22;
            if (i >= 0 && i < (int) folders.size())
            {
                const auto key = folders[(size_t) i].key;
                if ((key == "F" || key == "U") && e.x < fa.getX() + folders[(size_t) i].depth * 14 + 14)
                {
                    if (expanded.count (key)) expanded.erase (key); else expanded.insert (key);
                    buildFolders();
                }
                else folderKey = key;
                applyFilter();
            }
            return;
        }
        for (auto& c : chips)
            if (c.r.contains (e.getPosition()))
            {
                auto& sel = showTags ? selTags : selCats;
                if (sel.contains (c.text)) sel.removeString (c.text); else sel.add (c.text);
                applyFilter();
                return;
            }
        for (auto& [t, r] : tagRects)
            if (r.contains (e.getPosition())) { removeTag (t); return; }
        const auto v = view.getBounds();
        if (e.y >= 52 && e.y < 80 && e.x >= v.getX() && e.x < v.getRight())
            for (int c = 0; c < 5; ++c)
                if (e.x - v.getX() >= tableX[c] && e.x - v.getX() < tableX[c + 1])
                {
                    if (sortCol == c) sortAsc = ! sortAsc; else { sortCol = c; sortAsc = true; }
                    applyFilter();
                }
    }

    PanelContext& ctx;
    TinctureProcessor& proc;
    PresetTable table;
    Viewport view;
    TextEditor search;
    PanelButton clearSearch { "x" }, cog { "PRESETS" }, closeBtn { "CLOSE" }, catTab { "CATEGORIES" }, tagTab { "TAGS" }, descBtn { "EDIT" }, addTag { "+ TAG" };
    ChoiceDisplay rating, catBox;
    std::function<void()> onClose;
    std::vector<Folder> folders;
    std::set<String> expanded { "F", "U" };
    String folderKey;
    StringArray catChips, tagChips, selCats, selTags;
    std::vector<Chip> chips;
    std::vector<std::pair<String, Rectangle<int>>> tagRects;
    bool showTags = false;
    int ratingFilter = -1, sortCol = 0;
    bool sortAsc = true;
    std::vector<int> visible;
    int selected = -1, loaded = -1;
    String shownPreset;
};

void PresetTable::paint (Graphics& g)
{
    const auto& all = br.entries();
    const String current = br.proc.presetName();
    for (size_t i = 0; i < br.visible.size(); ++i)
    {
        const int idx = br.visible[i];
        const auto& e = all[(size_t) idx];
        const auto r = Rectangle<float> (0.0f, (float) i * rowH, (float) getWidth(), (float) rowH);
        const bool sel = idx == br.selected;
        g.setColour (sel ? colours::cream : (i % 2 ? colours::panel : colours::panel.brighter (0.04f)));
        g.fillRect (r);
        const Colour ink = sel ? colours::black : colours::ink;
        const bool isCurrent = e.name == current;
        auto col = [&] (int c) { return r.withX ((float) Browser::tableX[c]).withWidth ((float) (Browser::tableX[c + 1] - Browser::tableX[c] - 6)); };
        if (isCurrent)
        {
            g.setColour (colours::brand);
            g.fillRect (Rectangle<float> (1.0f, r.getY() + 5, 3.0f, rowH - 10.0f));
        }
        drawText (g, e.name, col (0), 13.0f, ink, isCurrent, Justification::centredLeft);
        drawText (g, br.location (e), col (1), 11.5f, sel ? colours::black : colours::inkDim, false, Justification::centredLeft);
        drawText (g, e.category, col (2), 11.5f, sel ? colours::black : colours::inkDim, false, Justification::centredLeft);
        drawText (g, br.dateOf (e), col (3), 11.5f, sel ? colours::black : colours::inkDim, false, Justification::centredLeft);
        for (int s = 0; s < 5; ++s)
            drawText (g, kStar, { (float) Browser::tableX[4] + (float) s * 18.0f, r.getY(), 18.0f, (float) rowH }, 14.0f,
                      s < e.rating ? (sel ? colours::black : colours::lcd) : (sel ? colours::creamShade.darker (0.3f) : colours::inkFaint), false);
    }
    if (br.visible.empty())
        drawText (g, "No presets match", { 0.0f, 20.0f, (float) getWidth(), 20.0f }, 13.0f, colours::inkDim, false);
}

void PresetTable::mouseDown (const MouseEvent& e)
{
    br.grabKeyboardFocus();
    const int row = e.y / rowH;
    if (row < 0 || row >= (int) br.visible.size()) return;
    const int idx = br.visible[(size_t) row];
    if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), br.rowMenu (idx)); return; }
    if (e.x >= Browser::tableX[4] && e.x < Browser::tableX[4] + 90)
    {
        br.setRating (idx, jlimit (1, 5, (e.x - Browser::tableX[4]) / 18 + 1));
        return;
    }
    br.select (idx);
    br.load (idx);
}

void PresetTable::mouseDoubleClick (const MouseEvent&) {}
} // namespace

std::unique_ptr<juce::Component> createPresetBrowser (PanelContext& ctx, std::function<void()> onClose)
{
    return std::make_unique<Browser> (ctx, std::move (onClose));
}
} // namespace tincture::ui
