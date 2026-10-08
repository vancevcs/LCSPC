"""Browse GTA texture dictionaries and push their textures into LCS.

Ported from the VCS fork's tools/txd_browser.py (gtavcspc 1193f89): the same three
panes, search, per-texture matching, auto-assign, unmatched and compare-packs
reviews, and resolution families, pointed at LCS. What the VCS docstring said
still holds; what changed for LCS:

- The sources are the VCS fork's sources folder (the PC upscale packs and the
  Definitive Edition dumps, ~123,000 textures) and the Android OBB, which lcsrw
  lists as dictionaries named android/<txd> and lcsmatch as the pack "android".
  The PC index is the VCS tool's own cache, adopted where the folder moved to.
- Install keeps the PSP texture's brightness ("keep PSP brightness", on by
  default; lcshd.colour_matched), writes DDS, and puts the entries in
  textures.ini's manual block (lcshd.set_manual), which lcshd bind and merge leave
  alone. A hash the hand-made HUD pack holds is refused.
- No aspect fitting: PPSSPP draws a replacement over the original's UV space
  whatever its shape (docs/lcs/hd-textures.md), so square art for a 2:1 texture
  goes in as it is.
- A dump too flat to tell candidates apart is never pre-picked (lcsmatch.MIN_DETAIL)
  and says "flat" on its row.
- No disc extractor and no "game art" target: those were VCS's.

Usage:
    python Tools/lcstxd.py
    python Tools/lcstxd.py <sources folder> <main.obb> --pack <TEXTURES/ULUS10041>
"""

import os
import queue
import sys
import threading
import traceback
from concurrent.futures import ThreadPoolExecutor

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from PIL import Image, ImageTk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lcsrw as txd   # noqa: E402
import lcsmatch       # noqa: E402

THUMB = 112
PREVIEW = 340
MATCH_THUMB = 64
MAX_RESULTS = 600          # a name search can hit thousands; show the first lot

BG = "#23262b"
PANEL = "#2b2f36"
FG = "#d8dce3"
DIM = "#8b93a1"
ACCENT = "#4a9eff"


def checkerboard(size, light=(102, 106, 114), dark=(78, 82, 90), square=8):
    """The transparency backdrop, so a cut-out reads as cut-out.

    Built by tiling one two-square block rather than by walking pixels -- this
    gets called for every thumbnail, and a Python loop over a 2048x2048 texture
    would dominate the whole browser.
    """
    w, h = size
    tile = Image.new("RGB", (square * 2, square * 2), light)
    dark_square = Image.new("RGB", (square, square), dark)
    tile.paste(dark_square, (square, 0))
    tile.paste(dark_square, (0, square))
    board = Image.new("RGB", (w, h))
    for y in range(0, h, square * 2):
        for x in range(0, w, square * 2):
            board.paste(tile, (x, y))
    return board


def fit(image, box):
    """Scale to fit `box`, nearest-neighbour when enlarging so texels stay crisp."""
    w, h = image.size
    if max(w, h) == 0:
        return image
    scale = box / max(w, h)
    size = (max(1, int(w * scale)), max(1, int(h * scale)))
    return image.resize(size, Image.NEAREST if scale > 1 else Image.LANCZOS)


def display(image, box, mode="checker"):
    """An RGBA texture, scaled to `box` and flattened for display.

    Scaling happens before compositing so the backdrop is only ever built at
    display size. Pillow premultiplies alpha when it resamples RGBA, so doing
    it in this order does not smear colour out of the transparent regions.
    """
    image = fit(image, box)
    if mode == "alpha":
        return image.getchannel("A").convert("RGB")
    if mode == "checker":
        back = checkerboard(image.size).convert("RGBA")
    else:
        fill = {"black": (0, 0, 0, 255), "white": (255, 255, 255, 255),
                "magenta": (255, 0, 255, 255)}[mode]
        back = Image.new("RGBA", image.size, fill)
    back.alpha_composite(image)
    return back.convert("RGB")


def blank(size, colour="#26292f"):
    """A placeholder image, because an empty tk.Label sizes in characters."""
    return Image.new("RGB", (size, size), colour)


class CompareWindow(tk.Toplevel):
    """A source texture beside the LCS texture it would replace, both large.

    Both are drawn at the same size however far apart their real resolutions
    are -- that is the point of the window. The PSP texture is enlarged
    nearest-neighbour so its texels stay square and countable, rather than
    being smoothed into looking closer to the PC art than it really is.
    """

    def __init__(self, master, src_image, src_caption, psp_image, psp_caption,
                 mode="checker", on_install=None, colour=True):
        super().__init__(master)
        self.title(f"{src_caption.splitlines()[0]}  ↔  "
                   f"{psp_caption.splitlines()[0]}")
        self.configure(bg=PANEL)
        self.geometry("1080x660")
        self.minsize(520, 400)

        self.sources = (src_image, psp_image)
        self.raw = src_image
        self.matched = None
        self.photos = [None, None]
        self.bg_mode = tk.StringVar(value=mode)
        # Shown the way it would be installed: LCS stores its textures darker than
        # any HD source, and the install scales the source to the PSP's average.
        self.colour = tk.BooleanVar(value=bool(colour))
        self._resize_job = None

        bar = ttk.Frame(self, padding=(10, 8))
        bar.pack(fill="x")
        for label in ("checker", "black", "white", "alpha"):
            ttk.Radiobutton(bar, text=label, value=label, variable=self.bg_mode,
                            command=self.render).pack(side="left")
        ttk.Checkbutton(bar, text="PSP brightness", variable=self.colour,
                        command=self.render).pack(side="left", padx=(12, 0))
        if on_install:
            # Close only when the install actually went through -- cancelling
            # the confirmation should leave you looking at the comparison.
            ttk.Button(bar, text="Install this replacement", style="Go.TButton",
                       command=lambda: on_install() and self.destroy()
                       ).pack(side="right")

        body = ttk.Frame(self, padding=(10, 0, 10, 10))
        body.pack(fill="both", expand=True)
        body.columnconfigure(0, weight=1, uniform="pane")
        body.columnconfigure(1, weight=1, uniform="pane")
        body.rowconfigure(1, weight=1)

        self.panels = []
        for column, (caption, image) in enumerate(
                ((src_caption, src_image), (psp_caption, psp_image))):
            head = tk.Label(body, text=caption, bg=PANEL, fg=FG, justify="left",
                            anchor="w", font=("Segoe UI", 8))
            head.grid(row=0, column=column, sticky="ew", padx=4, pady=(0, 6))
            panel = tk.Label(body, bg="#1c1f24", bd=0)
            panel.grid(row=1, column=column, sticky="nsew", padx=4)
            self.panels.append(panel)

        self.bind("<Configure>", self._on_resize)
        self.bind("<Escape>", lambda _e: self.destroy())
        self.after(60, self.render)

    def _on_resize(self, _event=None):
        # Re-rendering on every Configure event would resample both textures
        # dozens of times during a single drag of the window edge.
        if self._resize_job:
            self.after_cancel(self._resize_job)
        self._resize_job = self.after(120, self.render)

    def render(self):
        self._resize_job = None
        if self.colour.get() and self.matched is None:
            self.matched, _gains = lcsmatch._lcshd().colour_matched(self.raw, self.sources[1])
        self.sources = (self.matched if self.colour.get() else self.raw, self.sources[1])
        box = max(64, min(self.panels[0].winfo_width(),
                          self.panels[0].winfo_height()) - 8)
        for i, (panel, image) in enumerate(zip(self.panels, self.sources)):
            self.photos[i] = ImageTk.PhotoImage(
                display(image, box, self.bg_mode.get()))
            panel.configure(image=self.photos[i])


GRADES = (("strong", 0.05, "#5ad46a"), ("likely", 0.15, "#9ed36a"),
          ("possible", 0.25, "#d8c25a"), ("weak", 9.9, "#d3705a"))
# What a review window is for: its title, the filter it opens on, and whatever
# the footer needs to say first. The leftovers list is the same window over
# rows that all grade weak by definition, so it has to open showing them --
# "likely and better" over a page of hopeless rows shows nothing at all.
MODES = {
    "assign": ("Auto-assign — pick one replacement per LCS texture",
               "likely", ""),
    "unmatched": ("Unmatched — LCS textures nothing was found for", "weak",
                  "nothing here scored a real match, these are the closest the "
                  "search could come · "),
    # Not a variant of auto-assign but the opposite question. That one asks
    # what the whole source tree can do for the textures still wanting art;
    # this asks what one pack can do for all of them, installed included --
    # which is the only way to find out whether a pack that arrived after the
    # work was done would have been the better choice.
    "packs": ("Room for improvement — every LCS texture against chosen packs",
              "possible",
              "installed rows are shown too, with what they already carry "
              "beside the alternatives · nothing installed is pre-picked · "),
}
AUTO_PICK_BELOW = 0.05      # what gets chosen for you when the list first opens
# LCS: candidates this close to the closest are the same artwork, and of those the
# largest is the one picked for you. The closest is usually a copy at the PSP's own
# size (SA's 64x64 shad_car over a 256x256 one at the same distance), which gains
# nothing installed.
PREPICK_TIE = 0.01
ROWS_PER_PAGE = 24
ALT_THUMB = 76
PSP_THUMB = 64
# Canvas scroll units per wheel detent. A unit is a tenth of the visible height,
# so three of them is roughly two rows -- enough to feel responsive over a long
# review without overshooting the row you were reading.
WHEEL_UNITS = 3
ROW_BG = "#1c1f24"
CELL_BG = "#22262c"
FAMILY_BG = "#232a33"       # the banner over one family's rungs
FAMILY_EDGE = "#4d7ea8"     # the spine down the side of a family's rows
KIN = "#c9a227"             # artwork another rung of the family already took


def grade_of(dist):
    for name, limit, colour in GRADES:
        if dist < limit:
            return name, colour
    return GRADES[-1][0], GRADES[-1][2]


def de_packs(folders):
    """Source packs that are a Definitive Edition dump.

    Recognised by the manifest de_dump_textures.py writes beside the art rather
    than by folder name. The folder can be called anything, and a name test
    would quietly stop being right the first time one is renamed -- whereas a
    textures.csv carrying `src_width` is only ever written by that dumper.
    """
    found = set()
    for root in folders:
        try:
            names = os.listdir(root)
        except OSError:
            continue
        for name in names:
            manifest = os.path.join(root, name, "textures.csv")
            if not os.path.isfile(manifest):
                continue
            try:
                with open(manifest, "r", encoding="utf-8") as fh:
                    if "src_width" in (fh.readline() or ""):
                        found.add(name)
            except OSError:
                pass
    return found


class PackPicker(tk.Toplevel):
    """Which source packs a review should be scored against.

    Modal, and deliberately a list of what the index actually holds rather than
    a folder chooser: the packs are whatever the last scan found, so offering
    anything else would be offering something that cannot be scored without a
    reindex first.
    """

    def __init__(self, master, counts, preselect=()):
        super().__init__(master)
        self.title("Score against which packs?")
        self.configure(bg=PANEL)
        self.resizable(False, False)
        self.result = None
        preselect = set(preselect)

        ttk.Label(self, text="Only textures from the ticked packs are offered "
                             "as candidates.",
                  style="Dim.TLabel", padding=(12, 10, 12, 4)).pack(anchor="w")
        body = ttk.Frame(self, padding=(12, 0, 12, 6))
        body.pack(fill="both", expand=True)
        self.vars = {}
        for name in sorted(counts, key=lambda n: (n not in preselect, n.lower())):
            var = tk.BooleanVar(value=name in preselect)
            self.vars[name] = var
            label = f"{name or '(root)'}   ·   {counts[name]:,} textures"
            if name in preselect:
                label += "   ·   Definitive Edition dump"
            ttk.Checkbutton(body, text=label, variable=var).pack(anchor="w")

        foot = ttk.Frame(self, padding=(12, 4, 12, 12))
        foot.pack(fill="x")
        ttk.Button(foot, text="all", command=lambda: self._set(True)
                   ).pack(side="left")
        ttk.Button(foot, text="none", command=lambda: self._set(False)
                   ).pack(side="left", padx=(4, 0))
        ttk.Button(foot, text="Score", style="Go.TButton", command=self._ok
                   ).pack(side="right")
        ttk.Button(foot, text="Cancel", command=self.destroy
                   ).pack(side="right", padx=(0, 6))

        self.transient(master)
        self.grab_set()
        self.wait_window(self)

    def _set(self, state):
        for var in self.vars.values():
            var.set(state)

    def _ok(self):
        self.result = [n for n, v in self.vars.items() if v.get()]
        self.destroy()


class AutoAssignWindow(tk.Toplevel):
    """Every dumped LCS texture beside the candidates that could replace it.

    The whole cross product is scored at once (see lcsmatch.assign) and shown
    worst-last, so the top of the list is where the certain matches are and you
    can stop scrolling when they stop being obvious.

    Each row lays its candidates out side by side rather than one at a time,
    because with several upscale projects open the question is rarely "is this
    the right texture" -- usually every candidate is the right texture, and what
    is actually being chosen is whose upscale of it to take. That is a judgement
    you can only make by seeing them together, so a row offers one click to pick
    a winner and no way to pick two. lcsmatch.assign spreads the candidates
    across source packs so the row shows real alternatives instead of the same
    artwork four times over.

    Rows that are the same artwork at different resolutions are kept together
    rather than scattered through the ranking -- see lcsmatch.Families for how
    they are found. The family surfaces where its best-matching member would
    have come anyway, its other rungs follow immediately below it largest
    first, and a page ends early rather than cut one in half. Once a rung has
    been given some artwork the other rungs mark any candidate that is the same
    artwork, so choosing a deliberately cheaper version for the distant copy is
    a matter of not clicking the marked one.

    Anything under 0.05 starts with its best candidate chosen, because at that
    distance the two pictures are the same artwork; the rest start on nothing
    and are yours to judge. Nothing is written until Install.

    Rows are paginated rather than all built at once -- thousands of rows of
    live widgets is more than Tk will scroll smoothly -- and thumbnails are
    decoded on a worker pool and cached across pages, since a page of ten
    alternatives apiece is a couple of hundred rasters.
    """

    def __init__(self, master, dump, txds, rows, pack_dir, dump_dir, installed,
                 families=None, colour=True,
                 mode="assign", note="", hide_installed=True, auto_pick=True,
                 current=None):
        super().__init__(master)
        title, grade, self.hint = MODES.get(mode, MODES["assign"])
        self.mode = mode
        self.note = note
        self.title(title)
        self.configure(bg=PANEL)
        self.geometry("1460x920")
        self.minsize(1000, 560)

        self.dump = dump
        self.txds = txds
        self.all_rows = rows
        self.pack_dir = pack_dir
        self.dump_dir = dump_dir
        self.installed = installed          # hashes already in textures.ini
        self.families = families or lcsmatch.Families([], [], [])
        self.current = dict(current or {})  # hash -> the replacement it carries
        self.current_dims = {}              # hash -> that replacement's size

        self.by_index = dict(rows)
        # None means "no replacement chosen for this texture"; anything else is
        # an index into that row's candidate list. One value, one choice.
        #
        # A row that already carries a replacement is never pre-picked, even at
        # a distance that would normally decide it. A review that includes
        # installed textures is asking whether the new art is better than what
        # is there, and answering that on the user's behalf -- one Install
        # click away -- is how a finished pack gets quietly overwritten by
        # whatever happened to score three thousandths closer.
        #
        # LCS: nor is a dump too flat to tell candidates apart (lcsmatch.MIN_DETAIL),
        # where a near-zero distance says nothing.
        self.pick = {d: (self.best(d, c) if (auto_pick and c[0][1] < AUTO_PICK_BELOW
                                           and self.dump.hashes[d] not in self.installed
                                           and not self.dump.flat(d))
                         else None)
                     for d, c in rows}
        self.page = 0
        self.cancel = False

        # Whether a replacement is scaled to the PSP texture's average colour
        # before it is written (lcsmatch.install_many).
        self.colour = tk.BooleanVar(value=bool(colour))

        self.grade_filter = tk.StringVar(value=grade)
        self.hide_installed = tk.BooleanVar(value=bool(hide_installed))
        self.alt_count = tk.StringVar(value="6")
        self.group = tk.BooleanVar(value=True)
        self.families_only = tk.BooleanVar(value=False)

        self.thumbs = {}                    # cache key -> PhotoImage
        self.blanks = {}                    # size -> placeholder PhotoImage
        self.waiting = {}                   # cache key -> [labels to fill in]
        self.row_widgets = {}               # dump index -> its row Frame
        self.head_widgets = {}              # family id -> its banner Frame
        self.row_fid = {}                   # dump index -> family id, if grouped
        self.row_kin = {}                   # family id -> its rows, largest first
        self.starts = [0]                   # first row of each page
        self.inflight = set()
        self.results = queue.Queue()
        self.generation = 0
        self.pool = ThreadPoolExecutor(max_workers=min(8, (os.cpu_count() or 4)))

        self._build()
        self.refilter()
        self.drain_job = self.after(40, self._drain)
        self.protocol("WM_DELETE_WINDOW", self._close)

    def _close(self):
        # Cancel the pending drain before tearing the widgets down. Left
        # scheduled, it fires once more against destroyed labels and Tk reports
        # the failure to stderr as a background exception.
        if self.drain_job:
            self.after_cancel(self.drain_job)
            self.drain_job = None
        self.generation += 1
        self.pool.shutdown(wait=False)
        self.destroy()

    # ------------------------------------------------------------------ chrome

    def _build(self):
        bar = ttk.Frame(self, padding=(10, 8))
        bar.pack(fill="x")
        ttk.Label(bar, text="show:").pack(side="left")
        for label in ("strong", "likely", "possible", "weak"):
            ttk.Radiobutton(bar, text=f"{label} +", value=label,
                            variable=self.grade_filter,
                            command=self.refilter).pack(side="left")
        ttk.Checkbutton(bar, text="hide already installed",
                        variable=self.hide_installed,
                        command=self.refilter).pack(side="left", padx=(12, 0))
        ttk.Checkbutton(bar, text="group families", variable=self.group,
                        command=self.refilter).pack(side="left", padx=(12, 0))
        ttk.Checkbutton(bar, text="only families", variable=self.families_only,
                        command=self.refilter).pack(side="left", padx=(6, 0))

        ttk.Label(bar, text="alternatives").pack(side="left", padx=(16, 4))
        alts = ttk.Combobox(bar, width=4, state="readonly",
                            textvariable=self.alt_count,
                            values=("5", "6", "8", "10"))
        alts.pack(side="left")
        alts.bind("<<ComboboxSelected>>", lambda _e: self.render())

        ttk.Button(bar, text="best on page", command=lambda: self.set_page(True)
                   ).pack(side="left", padx=(16, 0))
        ttk.Button(bar, text="clear page", command=lambda: self.set_page(False)
                   ).pack(side="left", padx=(4, 0))
        ttk.Button(bar, text="clear all", command=self.clear_all
                   ).pack(side="left", padx=(4, 0))

        self.install_btn = ttk.Button(bar, text="Install", style="Go.TButton",
                                      command=self.install_checked)
        self.install_btn.pack(side="right")
        ttk.Checkbutton(bar, text="keep PSP brightness", variable=self.colour
                        ).pack(side="right", padx=(0, 10))
        self.summary = ttk.Label(bar, text="", style="Dim.TLabel")
        self.summary.pack(side="right", padx=(0, 12))

        if self.note:
            # Its own line rather than the summary label up in the bar: that
            # one belongs to the pick counter the moment a row is drawn.
            ttk.Label(self, text=self.note, style="Dim.TLabel",
                      padding=(10, 0, 10, 6)).pack(fill="x")

        wrap = ttk.Frame(self, padding=(10, 0))
        wrap.pack(fill="both", expand=True)
        self.canvas = tk.Canvas(wrap, bg=ROW_BG, bd=0, highlightthickness=0)
        scroll = ttk.Scrollbar(wrap, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.list = tk.Frame(self.canvas, bg=ROW_BG)
        self.canvas.create_window((0, 0), window=self.list, anchor="nw", tags="inner")
        self.list.bind("<Configure>", lambda _e: self.canvas.configure(
            scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>",
                         lambda e: self.canvas.itemconfigure("inner", width=e.width))
        # One binding on the window, not one per widget. Every widget inside a
        # Toplevel carries that Toplevel in its bindtags, so this catches the
        # wheel over rows, thumbnails and captions alike -- which per-widget
        # bindings never managed, leaving most of the page dead to the wheel.
        # Unlike bind_all it cannot reach the browser behind it or a compare
        # window opened on top.
        self.bind("<MouseWheel>", self._wheel)

        foot = ttk.Frame(self, padding=(10, 8))
        foot.pack(fill="x")
        self.prev_btn = ttk.Button(foot, text="◀ prev", command=lambda: self.turn(-1))
        self.prev_btn.pack(side="left")
        self.page_label = ttk.Label(foot, text="", style="Dim.TLabel")
        self.page_label.pack(side="left", padx=10)
        self.next_btn = ttk.Button(foot, text="next ▶", command=lambda: self.turn(1))
        self.next_btn.pack(side="left")
        ttk.Label(foot, text=self.hint +
                  "click an alternative to choose it · click it again "
                  "to clear · right-click to compare full size · ◆ ◇ "
                  "mark artwork another rung already took",
                  style="Dim.TLabel").pack(side="right")

    def _wheel(self, event):
        """Scroll the row list, wherever in the window the pointer happens to be.

        Windows reports one detent as a delta of 120; anything else comes from a
        precision wheel or trackpad sending partial steps, and is followed at
        whatever rate it arrives rather than rounded down to nothing.
        """
        steps = event.delta / 120 or (1 if event.delta > 0 else -1)
        self.canvas.yview_scroll(int(-steps * WHEEL_UNITS) or
                                 (-1 if steps > 0 else 1), "units")

    # ----------------------------------------------------------------- filters

    def refilter(self):
        limit = dict((n, l) for n, l, _c in GRADES)[self.grade_filter.get()]
        rows = [
            (d, cands) for d, cands in self.all_rows
            if cands[0][1] < limit
            and not (self.hide_installed.get() and self.dump.hashes[d] in self.installed)
        ]
        if self.group.get():
            # Orphans are dropped after grouping, not before: a texture whose
            # family exists in the index but whose other rungs were filtered
            # away is shown alone, and "only families" promising a comparison
            # that is not on the page would be a lie.
            self.rows = self._grouped(rows)
            if self.families_only.get():
                self.rows = [r for r in self.rows if r[0] in self.row_fid]
        else:
            self.row_fid, self.row_kin = {}, {}
            self.rows = [r for r in rows
                         if self.families.id(self.dump.hashes[r[0]]) is not None
                         ] if self.families_only.get() else rows
        self._paginate()
        self.page = 0
        self.render()

    def _grouped(self, rows):
        """Best-first, but with each family's rungs kept together.

        A family surfaces where its closest-matching member would have come in
        the plain ranking, and its other rungs follow immediately, largest
        first. That is the order the choice actually gets made in: you decide
        what the near texture deserves and grade the distant ones down from
        there, which is not a thing you can do with the 16x16 nine hundred rows
        further down.

        Membership is recomputed against what survived the filters rather than
        read straight off the index. A family whose other rungs are all already
        installed is a family of one, and drawing a banner over it would be
        promising a comparison that is not on the page.
        """
        held = {self.dump.hashes[d]: (d, cands) for d, cands in rows}
        self.row_fid, self.row_kin = {}, {}
        out, placed = [], set()
        for dump_idx, cands in rows:
            name = self.dump.hashes[dump_idx]
            if name in placed:
                continue
            kin = [k for k in self.families.kin(name) if k in held]
            if len(kin) < 2:
                out.append((dump_idx, cands))
                placed.add(name)
                continue
            fid = self.families.id(name)
            self.row_kin[fid] = [held[k][0] for k in kin]
            for k in kin:
                out.append(held[k])
                placed.add(k)
                self.row_fid[held[k][0]] = fid
        return out

    def fid(self, dump_idx):
        """The family this row is being shown as part of, or None."""
        return self.row_fid.get(dump_idx)

    def _paginate(self):
        """Page boundaries that never cut a family in half.

        A family split across a page turn is worse than not grouping at all --
        seeing the rungs at once is the whole point -- so a page ends early
        instead of in the middle of one. A family taller than a page is the one
        case that cannot be honoured, and is let overflow rather than looping.
        """
        self.starts, at = [], 0
        while at < len(self.rows):
            self.starts.append(at)
            stop = min(at + ROWS_PER_PAGE, len(self.rows))
            back = stop
            while at < back < len(self.rows) and self._same_family(back - 1, back):
                back -= 1
            at = back if back > at else stop
        self.starts = self.starts or [0]

    def _same_family(self, i, j):
        left = self.fid(self.rows[i][0])
        return left is not None and left == self.fid(self.rows[j][0])

    def pages(self):
        return len(self.starts)

    def turn(self, step):
        self.page = max(0, min(self.page + step, self.pages() - 1))
        self.render()

    def visible(self):
        page = min(self.page, len(self.starts) - 1)
        start = self.starts[page]
        stop = self.starts[page + 1] if page + 1 < len(self.starts) else len(self.rows)
        return self.rows[start:stop]

    def best(self, dump_idx, cands):
        """The candidate a row starts on: the largest of those tied with the closest.

        None when even that is no bigger than the PSP texture: the E3 build's
        textures and the stock PC ones are often the same art at the PSP's size,
        and putting one in gains nothing.
        """
        area = lambda k: self.txds.dims[cands[k][0]][0] * self.txds.dims[cands[k][0]][1]
        tied = [k for k, (_i, dist) in enumerate(cands) if dist - cands[0][1] <= PREPICK_TIE]
        k = max(tied, key=lambda k: (area(k), -k))
        w, h = self.dump.sizes[dump_idx]
        return k if area(k) > w * h else None

    def set_page(self, best):
        """Choose each visible row's best candidate (`best`), or clear the page."""
        for d, c in self.visible():
            self.pick[d] = self.best(d, c) if best else None
        self.render(keep_scroll=True)

    def clear_all(self):
        for d in self.pick:
            self.pick[d] = None
        self.render(keep_scroll=True)

    def choose(self, dump_idx, cand):
        """Pick one alternative for a row, or unpick it when it was already on.

        A row is not guaranteed to have as many candidates as the strip has
        room for -- plenty of LCS textures match only one or two source
        textures within range -- so the index is checked here rather than
        trusted, and the redraw is what would otherwise fail.
        """
        if cand is not None and not 0 <= cand < len(self.by_index[dump_idx]):
            return
        self.pick[dump_idx] = None if self.pick.get(dump_idx) == cand else cand
        fid = self.fid(dump_idx)
        if fid is None:
            self._redraw_row(dump_idx)
            return
        # The markers that say which rung already took a given piece of artwork
        # sit on the other rows of the family, so a pick here changes them too.
        here = {d for d, _c in self.visible()}
        for other in self.row_kin.get(fid, ()):
            if other in here:
                self._redraw_row(other)

    def chosen(self):
        return [(d, self.by_index[d][self.pick[d]][0])
                for d, _c in self.rows if self.pick.get(d) is not None]

    # ------------------------------------------------------------------ render

    def render(self, keep_scroll=False):
        """Rebuild the whole page.

        `keep_scroll` holds your place for anything that changes what the rows
        say rather than which rows they are. Turning the page or refiltering
        starts at the top, because the content underneath you is different; a
        page-wide tick does not, because it is not.
        """
        at = self.canvas.yview()[0] if keep_scroll else 0.0
        self.generation += 1
        for child in self.list.winfo_children():
            child.destroy()
        self.waiting = {}
        self.row_widgets = {}
        self.head_widgets = {}

        alts = int(self.alt_count.get())
        drawn = None
        for dump_idx, cands in self.visible():
            fid = self.fid(dump_idx)
            if fid is not None and fid != drawn:
                self.head_widgets[fid] = self._family_header(fid)
            drawn = fid
            self.row_widgets[dump_idx] = self._row(dump_idx, cands, alts)

        grouped = sum(len(k) for k in self.row_kin.values())
        note = f"{len(self.rows):,} LCS textures"
        if grouped:
            note += f"   ·   {grouped:,} in {len(self.row_kin):,} families"
        self.page_label.configure(
            text=f"page {self.page + 1} / {self.pages()}   ·   {note}")
        self.prev_btn.state(["!disabled"] if self.page else ["disabled"])
        self.next_btn.state(["!disabled"] if self.page < self.pages() - 1 else ["disabled"])
        self._count()
        # The scroll region only exists once the new rows have been laid out,
        # so restoring the position before that would clamp it to zero.
        self.list.update_idletasks()
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        self.canvas.yview_moveto(at)

    def _redraw_row(self, dump_idx):
        """Redraw one row in place, leaving the rest of the page alone.

        Choosing an alternative changes exactly one row. Rebuilding the page
        for it would throw away the scroll position, which when you are working
        down a list of several thousand means being thrown back to the top on
        every single click. Repacking just this row `before` its neighbour
        keeps the order without touching anything else.
        """
        order = [d for d, _c in self.visible()]
        old = self.row_widgets.get(dump_idx)
        if old is None or dump_idx not in order:
            self.render(keep_scroll=True)
            return
        # Whatever comes next visually, which is the next family's banner when
        # this row is the last rung of its own -- repacking before the row
        # underneath instead would slide it past that banner and into the
        # wrong family.
        after, mine = None, self.fid(dump_idx)
        for d in order[order.index(dump_idx) + 1:]:
            head = self.head_widgets.get(self.fid(d))
            if head is not None and self.fid(d) != mine:
                after = head
                break
            if d in self.row_widgets:
                after = self.row_widgets[d]
                break
        old.destroy()
        row = self._row(dump_idx, self.by_index[dump_idx], int(self.alt_count.get()))
        if after is not None:
            row.pack_configure(before=after)
        self.row_widgets[dump_idx] = row
        self._count()

    def _count(self):
        n = len(self.chosen())
        self.summary.configure(text=f"{n:,} chosen")
        self.install_btn.configure(text=f"Install {n:,}" if n else "Install")

    def _family_header(self, fid):
        """The banner over one family: what the ladder is and how far it goes."""
        kin = self.row_kin.get(fid, ())
        rungs, seen = [], set()
        for d in kin:
            size = self.families.sizes.get(self.dump.hashes[d])
            if size and size not in seen:
                seen.add(size)
                rungs.append(f"{size[0]}×{size[1]}")

        bar = tk.Frame(self.list, bg=FAMILY_BG)
        bar.pack(fill="x", pady=(8, 0))
        tk.Frame(bar, bg=FAMILY_EDGE, width=4).pack(side="left", fill="y")
        tk.Label(bar, text="  " + " · ".join(rungs), bg=FAMILY_BG, fg=FG,
                 font=("Segoe UI", 8, "bold")).pack(side="left", pady=2)
        tail = (f"same artwork, {len(kin)} textures — largest first, so the top "
                f"row is the one worth the best upscale")
        tk.Label(bar, text="   " + tail, bg=FAMILY_BG, fg=DIM,
                 font=("Segoe UI", 7)).pack(side="left", pady=2)
        return bar

    def _kin_note(self, dump_idx, txd_idx):
        """(caption, exact) when another rung already took this artwork.

        Matched on the texture's name rather than on the entry, because with
        several upscale projects open the same artwork exists once per pack and
        the whole question being asked is which of them to take. An exact
        repeat -- the same file, given to two rungs -- is worth saying louder
        than a sibling from a different pack, so the two are told apart.
        """
        fid = self.fid(dump_idx)
        if fid is None:
            return None
        name = self.txds.names[txd_idx].lower()
        for other in self.row_kin.get(fid, ()):
            chosen = self.pick.get(other)
            if other == dump_idx or chosen is None:
                continue
            taken = self.by_index[other][chosen][0]
            if self.txds.names[taken].lower() != name:
                continue
            w, h = self.families.sizes.get(self.dump.hashes[other], (0, 0))
            return f"{'◆' if taken == txd_idx else '◇'} {w}×{h}", taken == txd_idx
        return None

    def _row(self, dump_idx, cands, alts):
        """One LCS texture, its alternatives, and which of them is chosen.

        Only the first `alts` candidates get a cell, but the choice is an index
        into the full list: narrowing the strip must not quietly reindex a pick
        onto a different texture, nor throw one away. A choice that falls off
        the end of the strip is still installed, and says so.
        """
        hash_name = self.dump.hashes[dump_idx]
        strip = cands[:alts]
        chosen = self.pick.get(dump_idx)

        row = tk.Frame(self.list, bg=ROW_BG, pady=4)
        row.pack(fill="x", pady=(0, 3))

        # ---- a spine down the side, lit on the rung that leads the family
        fid = self.fid(dump_idx)
        kin = self.row_kin.get(fid, ()) if fid is not None else ()
        if fid is not None:
            tk.Frame(row, bg=FAMILY_EDGE if kin[0] == dump_idx else FAMILY_BG,
                     width=4).pack(side="left", fill="y")

        # ---- the LCS texture this row is about
        left = tk.Frame(row, bg=ROW_BG)
        left.pack(side="left", padx=(2, 0))
        target = tk.Label(left, bg=CELL_BG, cursor="hand2")
        target.pack()
        self._want(("psp", hash_name), target, PSP_THUMB)
        w, h = self.dump.sizes[dump_idx]
        tk.Label(left, text=f"{w}×{h}", bg=ROW_BG, fg=DIM,
                 font=("Segoe UI", 7)).pack()
        if self.dump.flat(dump_idx):
            tk.Label(left, text="flat: look", bg=ROW_BG, fg=KIN,
                     font=("Segoe UI", 6)).pack()
        if kin:
            rung = kin.index(dump_idx) + 1
            tk.Label(left, bg=ROW_BG, font=("Segoe UI", 7),
                     fg=FAMILY_EDGE if rung == 1 else DIM,
                     text=(f"largest of {len(kin)}" if rung == 1
                           else f"rung {rung} of {len(kin)}")).pack()
        tk.Label(left, text=hash_name[-12:], bg=ROW_BG, fg=DIM,
                 font=("Consolas", 6)).pack()
        if hash_name in self.installed:
            tk.Label(left, text="installed", bg=ROW_BG, fg=ACCENT,
                     font=("Segoe UI", 6)).pack()
        shown = cands[chosen] if chosen is not None else cands[0]
        target.bind("<Button-1>",
                    lambda _e: self.open_compare(dump_idx, shown[0], shown[1]))

        # ---- the replacement it already carries, when there is one to beat
        #
        # Without this the row says "installed" and leaves the actual question
        # unanswerable: whether a candidate is an improvement is a comparison
        # against the art in place, not against the 64px thing the PSP drew.
        current = self.current.get(hash_name)
        if current:
            self._current_cell(row, hash_name, current)

        tk.Label(row, text="←", bg=ROW_BG, fg=DIM,
                 font=("Segoe UI", 12)).pack(side="left", padx=6)

        # ---- the alternatives, side by side, one of which may be chosen
        for cand, (txd_idx, dist) in enumerate(strip):
            self._alt(row, dump_idx, cand, txd_idx, dist, cand == chosen)

        if chosen is not None:
            txd_idx = cands[chosen][0]
            note = tk.Frame(row, bg=ROW_BG)
            note.pack(side="left", padx=(10, 0), fill="x", expand=True)
            tk.Label(note, text=self.txds.names[txd_idx], bg=ROW_BG, fg=FG,
                     font=("Segoe UI", 9), anchor="w").pack(anchor="w", fill="x")
            tk.Label(note, text=self.txds.label(txd_idx), bg=ROW_BG, fg=DIM,
                     font=("Consolas", 7), anchor="w").pack(anchor="w", fill="x")
            if chosen >= len(strip):
                tk.Label(note, text=f"chosen: alternative {chosen + 1}, "
                                    f"beyond the {len(strip)} shown",
                         bg=ROW_BG, fg=ACCENT, font=("Segoe UI", 7),
                         anchor="w").pack(anchor="w", fill="x")
        return row

    def _current_cell(self, row, hash_name, path):
        """The replacement already installed against this texture.

        Deliberately shaped like a candidate cell and deliberately not one:
        same size and caption layout so the eye can compare it with the
        alternatives beside it, no click target so it cannot be chosen. What is
        already installed is the thing being measured against, not an option.
        """
        cell = tk.Frame(row, bg=CELL_BG, padx=2, pady=2,
                        highlightthickness=2, highlightbackground=ACCENT,
                        highlightcolor=ACCENT)
        cell.pack(side="left", padx=(8, 2))
        shot = tk.Label(cell, bg=CELL_BG)
        shot.pack()
        self._want(("cur", path), shot, ALT_THUMB)
        tk.Label(cell, text="installed", bg=CELL_BG, fg=ACCENT,
                 font=("Segoe UI", 7, "bold")).pack()
        dims = self._current_dims(hash_name, path)
        tk.Label(cell, text=(f"{dims[0]}×{dims[1]}" if dims else "?"),
                 bg=CELL_BG, fg=DIM, font=("Segoe UI", 6)).pack()
        tk.Label(cell, text=os.path.basename(os.path.dirname(path))[:14],
                 bg=CELL_BG, fg=DIM, font=("Segoe UI", 6),
                 wraplength=ALT_THUMB + 6).pack()

    def _current_dims(self, hash_name, path):
        """The installed replacement's size, read from the PNG header only.

        PIL opens lazily, so this is a header read per row rather than a
        decode, and the page only ever holds a couple of dozen rows. Cached
        anyway because paging back and forth would otherwise re-read them.
        """
        if hash_name not in self.current_dims:
            try:
                with Image.open(path) as img:
                    self.current_dims[hash_name] = img.size
            except Exception:
                self.current_dims[hash_name] = None
        return self.current_dims[hash_name]

    def _alt(self, row, dump_idx, cand, txd_idx, dist, selected):
        """One candidate cell: thumbnail, distance, and which pack it came from."""
        name, colour = grade_of(dist)
        edge = ACCENT if selected else ROW_BG
        cell = tk.Frame(row, bg=CELL_BG, padx=2, pady=2, cursor="hand2",
                        highlightthickness=2, highlightbackground=edge,
                        highlightcolor=edge)
        cell.pack(side="left", padx=2)

        shot = tk.Label(cell, bg=CELL_BG, cursor="hand2")
        shot.pack()
        self._want(("txd", self.txds.sources[txd_idx], self.txds.offsets[txd_idx]),
                   shot, ALT_THUMB)

        tk.Label(cell, text=f"{max(dist, 0.0):.3f}", bg=CELL_BG, fg=colour,
                 font=("Segoe UI", 7, "bold")).pack()
        w, h = self.txds.dims[txd_idx]
        pack = self.txds.pack(txd_idx) or self.txds.label(txd_idx)
        tk.Label(cell, text=f"{w}×{h}", bg=CELL_BG, fg=DIM,
                 font=("Segoe UI", 6)).pack()
        tk.Label(cell, text=pack[:14], bg=CELL_BG, fg=DIM, font=("Segoe UI", 6),
                 wraplength=ALT_THUMB + 6).pack()

        # Reserved for every cell in a family even when empty, so the rungs
        # keep a common height and do not jog up and down as picks are made.
        if self.fid(dump_idx) is not None:
            note = self._kin_note(dump_idx, txd_idx)
            text, exact = note if note else ("", False)
            tk.Label(cell, text=text, bg=CELL_BG, fg=ACCENT if exact else KIN,
                     font=("Segoe UI", 6)).pack()

        for widget in (cell, shot):
            widget.bind("<Button-1>", lambda _e: self.choose(dump_idx, cand))
            widget.bind("<Button-3>",
                        lambda _e: self.open_compare(dump_idx, txd_idx, dist))

    # ------------------------------------------------------- thumbnail loading

    def _want(self, key, label, size):
        """Show a thumbnail on `label`, decoding it off-thread if need be.

        Cached across pages by source and offset, so paging back and forth
        through a review costs nothing after the first visit.
        """
        photo = self.thumbs.get(key)
        if photo is not None:
            label.configure(image=photo)
            return
        if size not in self.blanks:
            self.blanks[size] = ImageTk.PhotoImage(blank(size))
        label.configure(image=self.blanks[size])
        self.waiting.setdefault(key, []).append(label)
        if key in self.inflight:
            return
        self.inflight.add(key)
        self.pool.submit(self._decode, self.generation, key, size)

    def _decode(self, gen, key, size):
        """Runs on a worker thread; never touches a Tk widget.

        Deliberately decodes whatever it was given even when the page has
        moved on. Abandoning it looked like the thrifty thing to do and was
        not: `_want` skips submitting a key that is already in flight, so a
        job dropped for being stale left the key marked in flight with nothing
        coming, and any label that asked for it again stayed blank for as long
        as the window was open. The work is cached either way, and a page
        skimmed past is usually a page come back to.
        """
        try:
            if key[0] == "psp":
                with Image.open(os.path.join(self.dump_dir, key[1] + ".png")) as src:
                    image = display(src.convert("RGBA"), size)
            elif key[0] == "cur":
                with Image.open(key[1]) as src:
                    image = display(src.convert("RGBA"), size)
            else:
                info = txd.TextureInfo("", "", 0, 0, "", 1, False,
                                       key[1], int(key[2]), 0, "")
                tex = info.load()
                image = display(tex.image(tex.level_for(size)), size)
        except Exception:
            image = blank(size)
        self.results.put((gen, key, image))

    def _drain(self):
        """Move finished thumbnails onto their widgets, on the main thread."""
        self.drain_job = None
        applied = 0
        while applied < 30:
            try:
                gen, key, image = self.results.get_nowait()
            except queue.Empty:
                break
            applied += 1
            self.inflight.discard(key)
            photo = ImageTk.PhotoImage(image)
            self.thumbs[key] = photo
            for label in self.waiting.pop(key, []):
                try:
                    label.configure(image=photo)
                except tk.TclError:
                    pass                    # that row was rebuilt under us
        if len(self.thumbs) > 4000:
            live = set(self.waiting)
            for key in [k for k in self.thumbs if k not in live][:2000]:
                del self.thumbs[key]
        self.drain_job = self.after(40, self._drain)

    # ----------------------------------------------------------------- actions

    def open_compare(self, dump_idx, txd_idx, dist):
        hash_name = self.dump.hashes[dump_idx]
        try:
            tex = self.txds.info(txd_idx).load()
            src_image = tex.image()
            with Image.open(os.path.join(self.dump_dir, hash_name + ".png")) as src:
                psp_image = src.convert("RGBA")
            caption = (f"{hash_name}   —   LCS (PSP dump)\n"
                       f"{psp_image.width}×{psp_image.height}   ·   "
                       f"match distance {dist:.3f}")
        except Exception as exc:
            messagebox.showerror("Could not open", str(exc))
            return
        CompareWindow(
            self, src_image,
            f"{tex.name}   —   source texture\n{src_image.width}×{src_image.height}"
            f"  {tex.format_name}   ·   {self.txds.label(txd_idx)}",
            psp_image, caption, colour=self.colour.get())

    def install_checked(self):
        picks = self.chosen()
        if not picks:
            return
        note = (" scaled to the brightness of the PSP texture each replaces,"
                if self.colour.get() else "")
        if not messagebox.askyesno(
                "Install replacements",
                f"Write {len(picks)} texture(s) into the pack{note} as DDS, and add "
                f"their textures.ini entries to the manual block?\n\n{self.pack_dir}"):
            return

        def job(dump_idx, txd_idx):
            return install_job(self.txds, txd_idx, self.dump.hashes[dump_idx])

        jobs = [job(d, t) for d, t in picks]
        by_hash = {self.dump.hashes[d]: d for d, _t in picks}

        self.cancel = False
        self.install_btn.configure(text="Cancel", command=self.stop_install)
        state = {"done": 0}
        result = {}

        def progress(done, _total):
            # Called from the encoder threads, so only stash the number here.
            # Tk widgets may not be touched from anything but the main thread.
            state["done"] = done

        # Read here: a Tk variable is the main thread's, like the widgets.
        colour = self.colour.get()

        def work():
            # Off the Tk thread on purpose. install_many blocks until the whole
            # batch is written, and running it inline would freeze the event
            # loop -- which would stop the progress updating and, worse, leave
            # the Cancel button unclickable for the entire install.
            result["out"] = lcsmatch.install_many(
                jobs, self.pack_dir, self.dump_dir, colour=colour,
                progress=progress, cancelled=lambda: self.cancel)

        worker = threading.Thread(target=work, daemon=True)
        worker.start()

        def poll():
            if worker.is_alive():
                self.summary.configure(
                    text=f"installing… {state['done']}/{len(jobs)}")
                self.after(120, poll)
                return
            self._installed(result.get("out", ({}, [])), by_hash)
        poll()

    def _installed(self, outcome, by_hash):
        done, failed = outcome
        for hash_name in done:
            self.installed.add(hash_name)
            self.pick[by_hash[hash_name]] = None
            if self.current:
                # A comparison review has to keep telling the truth after it
                # has been acted on: what a row now carries is what was just
                # written, not the file it was measured against.
                self.current[hash_name] = os.path.join(self.pack_dir, done[hash_name])
                self.current_dims.pop(hash_name, None)
                self.thumbs.pop(("cur", self.current[hash_name]), None)

        self.install_btn.configure(command=self.install_checked)
        self.refilter()
        note = f"installed {len(done)}"
        if self.cancel:
            note += " (cancelled)"
        if failed:
            note += f", {len(failed)} failed"
            messagebox.showerror("Some installs failed", "\n".join(failed[:10]))
        self.summary.configure(text=note)

    def stop_install(self):
        self.cancel = True
        self.summary.configure(text="stopping…")


def install_job(txds, txd_idx, hash_name):
    """One (hash, load, rel, comment) for lcsmatch.install_many."""
    pack = txds.pack(txd_idx)
    name = txds.names[txd_idx]
    rel = lcsmatch.install_path(hash_name, pack, name, txds.containers[txd_idx])

    def load(i=txd_idx):
        return txds.info(i).load().image()

    return hash_name, load, rel, f"<- {txds.label(txd_idx)}/{name} ({pack})"


def apply_theme(root):
    """The dark ttk theme every window in this pipeline is drawn in.

    A module-level function rather than a method, so a second application
    can wear the same chrome by importing one name. The alternative is a
    copy of forty lines of colours that drifts the first time one changes.
    """
    s = ttk.Style(root)
    try:
        s.theme_use("clam")
    except tk.TclError:
        pass
    s.configure(".", background=PANEL, foreground=FG, fieldbackground=PANEL,
                bordercolor="#3a3f48", lightcolor=PANEL, darkcolor=PANEL)
    s.configure("TFrame", background=PANEL)
    s.configure("TLabel", background=PANEL, foreground=FG)
    s.configure("Dim.TLabel", foreground=DIM)
    s.configure("Head.TLabel", font=("Segoe UI", 10, "bold"))
    s.configure("TButton", background="#3a3f48", foreground=FG, borderwidth=0,
                padding=(10, 5), focuscolor=PANEL)
    s.map("TButton", background=[("active", "#4a505b"), ("disabled", "#31353d")],
          foreground=[("disabled", "#5f6672")])
    s.configure("Go.TButton", background=ACCENT, foreground="#0d1117")
    s.map("Go.TButton", background=[("active", "#63b0ff"), ("disabled", "#31353d")])
    s.configure("TEntry", fieldbackground="#1c1f24", foreground=FG,
                insertcolor=FG, borderwidth=0, padding=4)
    s.configure("TCombobox", fieldbackground="#1c1f24", foreground=FG,
                arrowcolor=FG, borderwidth=0, padding=3)
    # A readonly combobox ignores the plain field colours and would
    # otherwise render as a white box on the dark panel.
    s.map("TCombobox",
          fieldbackground=[("readonly", "#1c1f24")],
          foreground=[("readonly", FG)],
          selectbackground=[("readonly", "#1c1f24")],
          selectforeground=[("readonly", FG)])
    root.option_add("*TCombobox*Listbox.background", "#1c1f24")
    root.option_add("*TCombobox*Listbox.foreground", FG)
    root.option_add("*TCombobox*Listbox.selectBackground", ACCENT)
    root.option_add("*TCombobox*Listbox.selectForeground", "#0d1117")
    s.configure("Vertical.TScrollbar", background="#3a3f48", troughcolor=BG,
                borderwidth=0, arrowcolor=DIM)
    s.configure("TRadiobutton", background=PANEL, foreground=FG)
    s.map("TRadiobutton", background=[("active", PANEL)])
    s.configure("TCheckbutton", background=PANEL, foreground=FG)
    s.map("TCheckbutton", background=[("active", PANEL)])
    s.configure("Horizontal.TProgressbar", background=ACCENT,
                troughcolor="#1c1f24", borderwidth=0, lightcolor=ACCENT,
                darkcolor=ACCENT)
    return s


class Browser(tk.Tk):
    def __init__(self, folders=None, pack=None):
        super().__init__()
        self.title("LCS textures")
        self.geometry("1500x900")
        self.configure(bg=BG)
        self.minsize(1050, 620)

        self.folders = list(folders or [])
        self.pack_dir = pack
        self.dumps = {}               # dictionary label -> [TextureInfo]
        self.order = []               # dictionary labels, sorted
        self.shown = []               # TextureInfo currently in the grid
        self.selected = None
        self.thumbs = {}              # id(info) -> PhotoImage
        self.match_thumbs = []
        self.preview_image = None
        self.cells = []
        self.bg_mode = tk.StringVar(value="checker")
        self.level = tk.IntVar(value=0)
        self.index = None             # lcsmatch.DumpIndex, built on demand
        self.txd_index = None         # lcsmatch.TxdIndex, built on demand
        self.match_dump = None        # folder the current candidates came from
        self.match_shown = []         # candidates render_matches last drew
        # Installs scale the source to the PSP texture's average colour: LCS
        # stores its textures darker than any HD source (docs/lcs/hd-textures.md).
        self.colour = tk.BooleanVar(value=True)

        self.results = queue.Queue()
        self.pool = ThreadPoolExecutor(max_workers=min(8, (os.cpu_count() or 4)))
        self.generation = 0           # invalidates in-flight thumbnail work

        self._build_style()
        self._build_ui()
        self.after(40, self._drain)

        if self.folders:
            self.load_sources(self.folders)
        else:
            self.after(120, self.choose_folder)

    # ------------------------------------------------------------------ chrome

    def _build_style(self):
        apply_theme(self)

    def _build_ui(self):
        bar = ttk.Frame(self, padding=(10, 8))
        bar.pack(fill="x")
        ttk.Button(bar, text="Open folder\u2026", command=self.choose_folder).pack(side="left")
        ttk.Button(bar, text="+ add", command=lambda: self.choose_folder(add=True)
                   ).pack(side="left", padx=(4, 0))
        ttk.Button(bar, text="Export this TXD\u2026",
                   command=self.export_current).pack(side="left", padx=(6, 0))
        ttk.Button(bar, text="Auto-assign", style="Go.TButton",
                   command=self.auto_assign).pack(side="left", padx=(6, 0))
        ttk.Button(bar, text="Unmatched…",
                   command=lambda: self.auto_assign(unmatched=True)
                   ).pack(side="left", padx=(4, 0))
        ttk.Button(bar, text="Compare packs…", command=self.compare_packs
                   ).pack(side="left", padx=(4, 0))
        ttk.Button(bar, text="Reindex", command=self.reindex
                   ).pack(side="left", padx=(4, 0))

        ttk.Label(bar, text="Search all textures:").pack(side="left", padx=(18, 6))
        self.search = ttk.Entry(bar, width=28)
        self.search.pack(side="left")
        self.search.bind("<KeyRelease>", lambda _e: self.after_idle(self.run_search))

        self.only_alpha = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="alpha only", variable=self.only_alpha,
                        command=self.refilter).pack(side="left", padx=(10, 0))
        ttk.Label(bar, text="min size").pack(side="left", padx=(12, 4))
        self.min_size = ttk.Combobox(bar, width=6, state="readonly",
                                     values=("any", "128", "256", "512", "1024"))
        self.min_size.set("any")
        self.min_size.pack(side="left")
        self.min_size.bind("<<ComboboxSelected>>", lambda _e: self.refilter())

        self.status = ttk.Label(bar, text="", style="Dim.TLabel")
        self.status.pack(side="right")

        panes = ttk.PanedWindow(self, orient="horizontal")
        panes.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        # ---- left: dictionaries
        left = ttk.Frame(panes, padding=(0, 0, 6, 0))
        panes.add(left, weight=0)
        ttk.Label(left, text="Texture dictionaries", style="Head.TLabel").pack(anchor="w")
        self.txd_filter = ttk.Entry(left)
        self.txd_filter.pack(fill="x", pady=(6, 4))
        self.txd_filter.bind("<KeyRelease>", lambda _e: self.refresh_txd_list())
        holder = ttk.Frame(left)
        holder.pack(fill="both", expand=True)
        self.txd_list = tk.Listbox(holder, bg="#1c1f24", fg=FG, bd=0,
                                   highlightthickness=0, selectbackground=ACCENT,
                                   selectforeground="#0d1117", activestyle="none",
                                   width=30, exportselection=False)
        bar_l = ttk.Scrollbar(holder, orient="vertical", command=self.txd_list.yview)
        self.txd_list.configure(yscrollcommand=bar_l.set)
        bar_l.pack(side="right", fill="y")
        self.txd_list.pack(side="left", fill="both", expand=True)
        self.txd_list.bind("<<ListboxSelect>>", self.on_pick_txd)

        # ---- middle: thumbnail grid
        middle = ttk.Frame(panes, padding=(6, 0))
        panes.add(middle, weight=1)
        self.grid_title = ttk.Label(middle, text="", style="Head.TLabel")
        self.grid_title.pack(anchor="w", pady=(0, 6))
        wrap = ttk.Frame(middle)
        wrap.pack(fill="both", expand=True)
        self.canvas = tk.Canvas(wrap, bg="#1c1f24", bd=0, highlightthickness=0)
        bar_m = ttk.Scrollbar(wrap, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=bar_m.set)
        bar_m.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.grid_frame = tk.Frame(self.canvas, bg="#1c1f24")
        self.canvas.create_window((0, 0), window=self.grid_frame, anchor="nw",
                                  tags="inner")
        self.grid_frame.bind("<Configure>", lambda _e: self.canvas.configure(
            scrollregion=self.canvas.bbox("all")))
        self.canvas.bind("<Configure>", self.on_canvas_resize)
        for widget in (self.canvas, self.grid_frame):
            widget.bind("<MouseWheel>", self.on_wheel)
        self.columns = 0

        # ---- right: detail
        right = ttk.Frame(panes, padding=(6, 0, 0, 0), width=390)
        panes.add(right, weight=0)
        self.detail = right
        self.blank_preview = ImageTk.PhotoImage(blank(PREVIEW, "#1c1f24"))
        self.blank_thumb = ImageTk.PhotoImage(blank(THUMB))
        self.preview = tk.Label(right, bg="#1c1f24", bd=0,
                                image=self.blank_preview)
        self.preview.pack(pady=(0, 8))

        opts = ttk.Frame(right)
        opts.pack(fill="x")
        for label, value in (("checker", "checker"), ("black", "black"),
                             ("white", "white"), ("alpha", "alpha")):
            ttk.Radiobutton(opts, text=label, value=value, variable=self.bg_mode,
                            command=self.render_preview).pack(side="left")
        self.mip = ttk.Combobox(opts, width=14, state="readonly", values=("mip 0",))
        self.mip.set("mip 0")
        self.mip.pack(side="right")
        self.mip.bind("<<ComboboxSelected>>", self.on_mip)

        self.info = ttk.Label(right, text="Select a texture", justify="left",
                              style="Dim.TLabel", anchor="nw")
        self.info.pack(fill="x", pady=(10, 8))

        row = ttk.Frame(right)
        row.pack(fill="x")
        self.save_btn = ttk.Button(row, text="Save PNG\u2026", command=self.save_png,
                                   state="disabled")
        self.save_btn.pack(side="left")
        self.copy_btn = ttk.Button(row, text="Copy name", command=self.copy_name,
                                   state="disabled")
        self.copy_btn.pack(side="left", padx=(6, 0))

        ttk.Separator(right, orient="horizontal").pack(fill="x", pady=12)
        head = ttk.Frame(right)
        head.pack(fill="x")
        ttk.Label(head, text="Install into LCS", style="Head.TLabel").pack(side="left")
        self.pack_label = ttk.Label(head, text="", style="Dim.TLabel")
        self.pack_label.pack(side="right")

        prow = ttk.Frame(right)
        prow.pack(fill="x", pady=(6, 6))
        self.match_btn = ttk.Button(prow, text="Find LCS matches",
                                    command=self.find_matches, state="disabled")
        self.match_btn.pack(side="left")
        ttk.Button(prow, text="Set pack\u2026",
                   command=self.choose_pack).pack(side="left", padx=(6, 0))

        arow = ttk.Frame(right)
        arow.pack(fill="x", pady=(0, 6))
        ttk.Checkbutton(arow, text="keep PSP brightness", variable=self.colour
                        ).pack(side="left")
        ttk.Label(arow, text="written as DDS, into the manual block",
                  style="Dim.TLabel", font=("Segoe UI", 7)).pack(side="left", padx=(6, 0))

        mholder = ttk.Frame(right)
        mholder.pack(fill="both", expand=True)
        self.match_canvas = tk.Canvas(mholder, bg="#1c1f24", bd=0,
                                      highlightthickness=0, height=190)
        bar_r = ttk.Scrollbar(mholder, orient="vertical",
                              command=self.match_canvas.yview)
        self.match_canvas.configure(yscrollcommand=bar_r.set)
        bar_r.pack(side="right", fill="y")
        self.match_canvas.pack(side="left", fill="both", expand=True)
        self.match_frame = tk.Frame(self.match_canvas, bg="#1c1f24")
        self.match_canvas.create_window((0, 0), window=self.match_frame, anchor="nw")
        self.match_frame.bind("<Configure>", lambda _e: self.match_canvas.configure(
            scrollregion=self.match_canvas.bbox("all")))
        self.match_canvas.bind("<MouseWheel>", lambda e: self.match_canvas.yview_scroll(
            -1 if e.delta > 0 else 1, "units"))

        self.set_pack(self.pack_dir)

    # ------------------------------------------------------------- folder load

    def choose_folder(self, add=False):
        picked = filedialog.askdirectory(
            title="Folder of .txd files or IMG archives")
        if picked:
            self.load_sources(self.folders + [picked] if add else [picked])

    def load_sources(self, folders):
        """Scan one or more game folders, loose .txd and IMG archives alike."""
        failed = []
        found = {}
        self.scans = {}
        for folder in folders:
            self.status.configure(text=f"scanning {os.path.basename(folder.rstrip(chr(92) + '/'))}…")
            self.update_idletasks()
            infos = txd.scan_tree_cached(folder, on_error=failed.append,
                                         rebuild=getattr(self, "_rescan", False))
            self.scans[folder] = infos
            for info in infos:
                found.setdefault(info.label, []).append(info)
        if not found:
            messagebox.showwarning(
                "Nothing found",
                "No .txd files or IMG archives with textures under\n"
                + "\n".join(folders))
            return

        self.folders = list(folders)
        self.dumps = found
        self.order = sorted(found)
        total = sum(len(v) for v in found.values())
        note = f"  ({len(failed)} unreadable)" if failed else ""
        self.status.configure(
            text=f"{len(self.order):,} dictionaries \u00b7 {total:,} textures{note}")
        self.title("LCS textures \u2014 " + "  +  ".join(
            "android" if txd.is_obb(f) else os.path.basename(f.rstrip("/\\")) or f
            for f in folders))
        # A new source set invalidates the signature index built from the old one.
        self.txd_index = None
        self.refresh_txd_list()

    def refresh_txd_list(self):
        needle = self.txd_filter.get().strip().lower()
        self.txd_list.delete(0, "end")
        self.visible_txds = [k for k in self.order if needle in k.lower()]
        for key in self.visible_txds:
            name = key[:-4] if key.lower().endswith(".txd") else key
            self.txd_list.insert("end", f"  {name}   ({len(self.dumps[key])})")
        if self.visible_txds:
            self.txd_list.selection_set(0)
            self.on_pick_txd()

    # -------------------------------------------------------------- populating

    def on_pick_txd(self, _event=None):
        sel = self.txd_list.curselection()
        if not sel:
            return
        self.search.delete(0, "end")
        key = self.visible_txds[sel[0]]
        name = key[:-4] if key.lower().endswith(".txd") else key
        every = self.dumps[key]
        shown = [i for i in every if self._passes(i)]
        note = "" if len(shown) == len(every) else f" of {len(every)}"
        self.show(shown, f"{name}  \u00b7  {len(shown)}{note} textures")

    def _passes(self, info):
        """The toolbar's size and alpha filters, applied to one texture."""
        floor = 0 if self.min_size.get() == "any" else int(self.min_size.get())
        if max(info.width, info.height) < floor:
            return False
        return info.has_alpha or not self.only_alpha.get()

    def refilter(self):
        """Reapply those filters to whichever view is currently showing."""
        if self.search.get().strip():
            self.run_search()
        else:
            self.on_pick_txd()

    def run_search(self):
        needle = self.search.get().strip().lower()
        if not needle:
            self.on_pick_txd()
            return
        hits = [info for path in self.order for info in self.dumps[path]
                if needle in info.name.lower() and self._passes(info)]
        label = f"\u201c{needle}\u201d  \u00b7  {len(hits)} match(es)"
        if len(hits) > MAX_RESULTS:
            label += f", showing first {MAX_RESULTS}"
            hits = hits[:MAX_RESULTS]
        self.show(hits, label)

    def show(self, infos, title):
        """Rebuild the thumbnail grid for a list of TextureInfo."""
        self.generation += 1
        self.shown = list(infos)
        self.grid_title.configure(text=title)
        for child in self.grid_frame.winfo_children():
            child.destroy()
        self.cells = []
        self.canvas.yview_moveto(0)

        for info in self.shown:
            cell = tk.Frame(self.grid_frame, bg="#1c1f24", padx=4, pady=4,
                            highlightthickness=2, highlightbackground="#1c1f24")
            thumb = tk.Label(cell, bg="#26292f", image=self.blank_thumb)
            thumb.pack()
            caption = tk.Label(cell, text=self._caption(info), bg="#1c1f24",
                               fg=DIM, font=("Segoe UI", 7), wraplength=THUMB + 8,
                               justify="center")
            caption.pack(pady=(3, 0))
            for widget in (cell, thumb, caption):
                widget.bind("<Button-1>", lambda _e, i=info: self.select(i))
                widget.bind("<MouseWheel>", self.on_wheel)
            self.cells.append((cell, thumb, info))

        self.relayout(force=True)
        self.request_thumbs()

    def _caption(self, info):
        name = info.name if len(info.name) <= 22 else info.name[:20] + "\u2026"
        return f"{name}\n{info.width}\u00d7{info.height} {info.format_name}"

    def on_canvas_resize(self, event):
        self.canvas.itemconfigure("inner", width=event.width)
        self.relayout()

    def relayout(self, force=False):
        width = max(self.canvas.winfo_width(), 100)
        columns = max(1, width // (THUMB + 22))
        if columns == self.columns and not force:
            return
        self.columns = columns
        for i, (cell, _thumb, _info) in enumerate(getattr(self, "cells", [])):
            cell.grid(row=i // columns, column=i % columns, padx=3, pady=3)

    def on_wheel(self, event):
        self.canvas.yview_scroll(-1 if event.delta > 0 else 1, "units")

    # ---------------------------------------------------------- thumbnail work

    def request_thumbs(self):
        gen = self.generation
        for _cell, thumb, info in self.cells:
            key = id(info)
            if key in self.thumbs:
                thumb.configure(image=self.thumbs[key])
                continue
            self.pool.submit(self._decode_thumb, gen, info)

    def _decode_thumb(self, gen, info):
        """Runs on a worker thread; never touches a Tk widget."""
        if gen != self.generation:
            return
        try:
            tex = info.load()
            image = tex.image(tex.level_for(THUMB))
            self.results.put((gen, id(info), display(image, THUMB), None))
        except Exception as exc:
            self.results.put((gen, id(info), None, str(exc)))

    def _drain(self):
        """Move finished thumbnails onto the widgets, on the main thread."""
        applied = 0
        while applied < 40:
            try:
                gen, key, image, error = self.results.get_nowait()
            except queue.Empty:
                break
            applied += 1
            if gen != self.generation or image is None:
                continue
            photo = ImageTk.PhotoImage(image)
            self.thumbs[key] = photo
            for _cell, thumb, info in self.cells:
                if id(info) == key:
                    thumb.configure(image=photo)
                    break
        if len(self.thumbs) > 3000:
            # Bound the cache, but never evict a thumbnail that is on screen --
            # dropping its PhotoImage would blank the cell it is drawn in.
            keep = {id(info) for _c, _t, info in self.cells}
            for key in [k for k in self.thumbs if k not in keep][:1500]:
                del self.thumbs[key]
        self.after(40, self._drain)

    # ------------------------------------------------------------------ detail

    def select(self, info):
        self.selected = info
        for cell, _thumb, other in self.cells:
            cell.configure(highlightbackground=ACCENT if other is info else "#1c1f24")
        try:
            self.texture = info.load()
        except Exception as exc:
            self.info.configure(text=f"Could not read this raster:\n{exc}")
            return

        self.mip.configure(values=[f"mip {i}  {w}\u00d7{h}"
                                   for i, (w, h, _) in enumerate(self.texture.mips)])
        self.mip.current(0)
        self.level.set(0)
        self.render_preview()

        lo, hi = self.texture.alpha_range()
        alpha = ("opaque" if lo == 255 else
                 "1-bit cut-out" if lo == 0 and hi == 255 and self.texture.compression == 1
                 else f"alpha {lo}\u2013{hi}")
        self.info.configure(text=(
            f"{self.texture.name}\n"
            f"{info.label}\n\n"
            f"{self.texture.width} \u00d7 {self.texture.height}   "
            f"{self.texture.format_name}   {self.texture.levels} mip levels\n"
            f"{alpha}   \u00b7   {self.texture.data_size / 1024:.0f} KB in the TXD"
            + (f"\nmask: {self.texture.mask}" if self.texture.mask else "")))
        for btn in (self.save_btn, self.copy_btn, self.match_btn):
            btn.configure(state="normal")

    def on_mip(self, _event=None):
        self.level.set(self.mip.current())
        self.render_preview()

    def render_preview(self):
        if not self.selected:
            return
        image = self.texture.image(self.level.get())
        self.preview_image = ImageTk.PhotoImage(
            display(image, PREVIEW, self.bg_mode.get()))
        self.preview.configure(image=self.preview_image)

    def copy_name(self):
        if self.selected:
            self.clipboard_clear()
            self.clipboard_append(self.selected.name)

    def save_png(self):
        if not self.selected:
            return
        dest = filedialog.asksaveasfilename(
            defaultextension=".png", filetypes=[("PNG", "*.png")],
            initialfile=f"{self.texture.name}.png")
        if dest:
            self.texture.image(self.level.get()).save(dest)
            self.status.configure(text=f"saved {os.path.basename(dest)}")

    def export_current(self):
        if not self.shown:
            return
        dest = filedialog.askdirectory(title="Export the textures now listed")
        if not dest:
            return
        written, failed = 0, 0
        for info in self.shown:
            try:
                tex = info.load()
                stem = "".join(c if c.isalnum() or c in "-_." else "_" for c in tex.name)
                tex.image().save(os.path.join(dest, f"{stem}.png"))
                written += 1
            except Exception:
                failed += 1
        self.status.configure(
            text=f"exported {written} PNG" + (f", {failed} failed" if failed else ""))

    # ------------------------------------------------------------ LCS matching

    def set_pack(self, path):
        self.pack_dir = path
        if path and os.path.isdir(path):
            self.pack_label.configure(text=os.path.basename(path.rstrip("/\\")))
        else:
            self.pack_label.configure(text="no pack set")

    def choose_pack(self):
        picked = filedialog.askdirectory(
            title="PPSSPP texture pack folder (the one holding textures.ini)")
        if picked:
            self.set_pack(picked)
            self.index = None
            self.match_dump = None

    def _dump_dir(self):
        """Where the hash-named PNGs live inside the pack."""
        if not self.pack_dir:
            return None
        candidate = os.path.join(self.pack_dir, "new")
        return candidate if os.path.isdir(candidate) else self.pack_dir

    def find_matches(self):
        if not self.selected:
            return
        dump = self._dump_dir()
        if not dump or not os.path.isdir(dump):
            messagebox.showinfo(
                "No dump folder",
                "Set the PPSSPP pack folder first.\n\nIt is the folder holding "
                "textures.ini; the dumped textures are expected in its 'new' "
                "subfolder.")
            return

        if self.index is None:
            self.status.configure(text="indexing the LCS dump\u2026")
            self.update_idletasks()
            try:
                self.index = lcsmatch.DumpIndex.cached(dump)
            except Exception as exc:
                messagebox.showerror("Index failed", str(exc))
                return
            self.status.configure(text=f"{len(self.index)} dumped LCS textures indexed")

        sig = lcsmatch.signature(self.texture.image(self.texture.level_for(128)))
        candidates = self.index.search(sig, 8)
        self.render_matches(candidates, dump)

    def render_matches(self, candidates, dump):
        for child in self.match_frame.winfo_children():
            child.destroy()
        self.match_thumbs = []
        # Remember where these came from, so opening one later cannot disagree
        # with where they were found if the pack has been repointed since.
        self.match_dump = dump
        self.match_shown = list(candidates)

        if not candidates:
            tk.Label(self.match_frame, text="  no candidates", bg="#1c1f24",
                     fg=DIM).pack(anchor="w")
            return

        tk.Label(self.match_frame, text="  click a match to compare it full size",
                 bg="#1c1f24", fg=DIM, font=("Segoe UI", 7)).pack(anchor="w")

        for hash_name, size, dist in candidates:
            row = tk.Frame(self.match_frame, bg="#1c1f24", pady=3)
            row.pack(fill="x", padx=4)
            clickable = [row]
            try:
                with Image.open(os.path.join(dump, hash_name + ".png")) as src:
                    thumb = display(src.convert("RGBA"), MATCH_THUMB)
                photo = ImageTk.PhotoImage(thumb)
                self.match_thumbs.append(photo)
                shot = tk.Label(row, image=photo, bg="#1c1f24", cursor="hand2")
                shot.pack(side="left")
                clickable.append(shot)
            except Exception:
                tk.Label(row, text="?", bg="#26292f", fg=DIM,
                         width=8, height=4).pack(side="left")

            # Under ~0.05 the two pictures are all but identical; past ~0.25 the
            # match is a guess and wants eyes on it before anything is written.
            quality = ("strong" if dist < 0.05 else
                       "likely" if dist < 0.15 else
                       "possible" if dist < 0.25 else "weak")
            colour = {"strong": "#5ad46a", "likely": "#9ed36a",
                      "possible": "#d8c25a", "weak": "#d3705a"}[quality]
            text = tk.Frame(row, bg="#1c1f24", cursor="hand2")
            text.pack(side="left", padx=(8, 0), fill="x", expand=True)
            grade = tk.Label(text, text=f"{quality}   d={dist:.3f}", bg="#1c1f24",
                             fg=colour, font=("Segoe UI", 8, "bold"), cursor="hand2")
            grade.pack(anchor="w")
            shape = f"{size[0]}\u00d7{size[1]}"
            detail = tk.Label(text, text=f"{shape}\n{hash_name}",
                              bg="#1c1f24", fg=DIM, font=("Consolas", 7),
                              justify="left", cursor="hand2")
            detail.pack(anchor="w")
            ttk.Button(row, text="Install", width=8,
                       style="Go.TButton",
                       command=lambda h=hash_name: self.install(h)).pack(side="right")

            clickable += [text, grade, detail]
            for widget in clickable:
                widget.bind("<Button-1>",
                            lambda _e, h=hash_name, d=dist: self.compare(h, d))

    def installed_paths(self):
        """Hash -> the replacement PNG the pack's textures.ini points at.

        Only [hashes] counts. Every section of the file is key = value --
        [options] holds `version`, `hash` and `ignoreMipmap`, and PPSSPP keeps
        [hashranges] and [filtering] after the entries -- so reading it
        straight through counts settings as though they were textures, which
        the leftovers headline reports out loud.

        The path is kept, not just the key, because "is this one done" and
        "done with what" are different questions and the second is the one a
        comparison against a new pack has to answer.
        """
        ini = os.path.join(self.pack_dir or "", "textures.ini")
        found = {}
        if not os.path.exists(ini):
            return found
        in_hashes = False
        with open(ini, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line.startswith("["):
                    in_hashes = line.lower() == "[hashes]"
                elif in_hashes and line and "=" in line \
                        and not line.startswith(("#", ";")):
                    key, _, rel = line.partition("=")
                    # An empty value is "no HD": done, with nothing to show.
                    found[key.strip()] = os.path.join(
                        self.pack_dir, rel.strip().replace("/", os.sep)) if rel.strip() else ""
        return found

    def installed_hashes(self):
        """Hashes already carrying a replacement in the pack's textures.ini."""
        return set(self.installed_paths())

    def reindex(self):
        """Rescan the sources and index whatever the scan finds new.

        Scans and signatures are both cached per source and keyed on its path,
        because checking the contents means walking well over a hundred thousand
        textures on every launch. That is the right trade until a pack is added
        to a source already scanned -- the new art is then invisible, with no way
        to tell from the outside. This is that way. Only the new textures are
        decoded (TxdIndex.top_up); a texture changed in place keeps its old
        signature until its source's .txd_index_*.npz is deleted.
        """
        if not messagebox.askyesno(
                "Reindex sources",
                "Rescan every source and index the textures that are new?\n\n"
                "Needed after adding textures to a source that has already been "
                "scanned. The rescan takes a few minutes over the whole tree."):
            return
        self.index = None
        self.txd_index = None
        self.match_dump = None
        self._rescan = True
        try:
            self.load_sources(self.folders)
        finally:
            self._rescan = False

        def note(text):
            self.status.configure(text=text)
            self.update_idletasks()

        try:
            note("indexing new source textures…")
            self.txd_index = lcsmatch.TxdIndex.cached(
                self.folders, scans=self.scans,
                progress=lambda i, n: note(f"indexing new source textures… {i:,}/{n:,}"))
        except Exception:
            messagebox.showerror("Reindex failed", traceback.format_exc(limit=3))
            return
        note(f"{len(self.txd_index):,} source textures indexed")

    def _review_ready(self, dump_dir):
        """Whether a review can run at all, complaining in the right words."""
        if not self.folders or not dump_dir or not os.path.isdir(dump_dir):
            messagebox.showinfo(
                "Not ready",
                "This needs at least one source folder and a pack whose "
                "'new' subfolder holds the dumped textures.")
            return False
        return True

    def _review_targets(self, note, dump_dir):
        """The left-hand column of a review: the dump, with the sources indexed too."""
        if self.index is None:
            note("indexing the LCS dump…")
            self.index = lcsmatch.DumpIndex.cached(
                dump_dir,
                progress=lambda i, n: note(f"indexing the LCS dump… {i}/{n}"))
        if self.txd_index is None:
            note("indexing the source textures… (first run is slow)")
            self.txd_index = lcsmatch.TxdIndex.cached(
                self.folders, scans=getattr(self, "scans", None),
                progress=lambda i, n: note(
                    f"indexing the source textures… {i:,}/{n:,}"))
        return self.index, 0

    def compare_packs(self):
        """Score every LCS texture against a chosen subset of the source packs.

        Auto-assign asks what the whole tree can do for the textures still
        wanting art. This asks what *one set of packs* can do for all of them,
        installed ones included -- which is the question that arrives with a
        pack that landed after the work was already done. Whether the new art
        beats what a texture already carries cannot be answered by a search
        that hides those rows, so this one shows them, puts the installed
        replacement next to the alternatives, and pre-picks none of them.

        Scoring is a subset of the index already in memory rather than a fresh
        index over the chosen folders: same signatures, no second walk, and the
        distances stay directly comparable with what auto-assign reported.
        """
        dump_dir = self._dump_dir()
        if not self._review_ready(dump_dir):
            return

        def note(text):
            self.status.configure(text=text)
            self.update_idletasks()

        try:
            prepared = self._review_targets(note, dump_dir)
            if prepared is None:
                note("")
                return
            targets, _ = prepared
        except Exception:
            messagebox.showerror("Failed", traceback.format_exc(limit=3))
            note("")
            return

        counts = self.txd_index.pack_counts()
        if not counts:
            messagebox.showinfo("No packs", "The source index is empty.")
            return
        chosen = PackPicker(self, counts, preselect=de_packs(self.folders)).result
        if not chosen:
            return

        try:
            subset = self.txd_index.only_packs(chosen)
            if not len(subset):
                messagebox.showinfo("Nothing to score",
                                    "Those packs hold no indexed textures.")
                return
            note(f"scoring every LCS texture against {len(subset):,} textures…")
            rows = lcsmatch.assign(
                targets, subset, top=10, per_pack=2,
                progress=lambda i, n: note(f"scoring… {i:,}/{n:,}"))
            note("grouping the dump into resolution families…")
            fams = lcsmatch.Families.cached(
                self.index, dump_dir,
                progress=lambda i, n: note(
                    f"grouping the dump into resolution families… {i:,}/{n:,}"))
        except Exception:
            messagebox.showerror("Failed", traceback.format_exc(limit=3))
            note("")
            return

        current = self.installed_paths()
        installed = set(current)
        current = {h: p for h, p in current.items() if os.path.exists(p)}
        covered = sum(1 for d, _c in rows if targets.hashes[d] in installed)
        summary = (f"{len(rows):,} proposals from {len(targets):,} target × "
                   f"{len(subset):,} source textures in "
                   f"{len(chosen)} pack(s)   ·   {covered:,} of them already "
                   f"carry a replacement")
        note(summary)
        if not rows:
            messagebox.showinfo(
                "Nothing to propose",
                "No texture in those packs came close to any LCS texture.")
            return
        AutoAssignWindow(self, targets, subset, rows, self.pack_dir, dump_dir,
                         installed, fams, colour=self.colour.get(),
                         mode="packs", hide_installed=False, auto_pick=True,
                         current=current,
                         note=f"scored against: {', '.join(sorted(chosen))}")

    def auto_assign(self, unmatched=False):
        """Score every replaceable LCS texture against every source texture.

        `unmatched` turns the same pass into its own complement. The ordinary
        review only proposes what came within lcsmatch.MAX_DISTANCE, so once a
        pack is half done the textures still wanting attention are exactly the
        ones it has never shown: nothing installed against them, and nothing
        close enough to have been offered. Those are scored with no distance
        limit at all and reviewed in the same window, opened on the weak filter
        because by definition that is the only grade any of them can hold.
        Every row there is a guess -- none is pre-picked, and nothing is
        written without a click.

        The row is keyed by hash, because that is the only thing a replacement
        can be installed against.
        """
        dump_dir = self._dump_dir()
        if not self._review_ready(dump_dir):
            return

        def note(text):
            self.status.configure(text=text)
            self.update_idletasks()

        try:
            prepared = self._review_targets(note, dump_dir)
            if prepared is None:
                note("")
                return
            targets, _ = prepared

            note("scoring every pair…")
            rows = lcsmatch.assign(
                targets, self.txd_index, top=10, per_pack=2,
                # No limit in leftovers mode: a row is wanted there precisely
                # because nothing came close, so a cutoff would empty the very
                # list it is meant to fill.
                max_distance=float("inf") if unmatched else lcsmatch.MAX_DISTANCE,
                progress=lambda i, n: note(f"scoring every pair… {i:,}/{n:,}"))
            installed = self.installed_hashes()
            if unmatched:
                rows = lcsmatch.leftovers(rows, targets, installed)
            # Grouped off the dump rather than off `targets`: the ladder is a
            # property of what the PSP drew, and in art mode `targets` carries
            # the extracted sizes instead. Both are keyed by hash, so the
            # grouping lands on either.
            note("grouping the dump into resolution families…")
            fams = lcsmatch.Families.cached(
                self.index, dump_dir,
                progress=lambda i, n: note(
                    f"grouping the dump into resolution families… {i:,}/{n:,}"))
        except Exception:
            messagebox.showerror("Auto-assign failed", traceback.format_exc(limit=3))
            note("")
            return

        if unmatched:
            summary = (f"{len(rows):,} of {len(targets):,} LCS textures have no "
                       f"replacement and nothing close enough to propose "
                       f"({len(installed):,} already installed)")
        else:
            summary = (f"{len(rows):,} proposals from {len(targets):,} target × "
                       f"{len(self.txd_index):,} source textures")
        if len(fams):
            summary += (f"   ·   {len(fams.of):,} of the dump group into "
                        f"{len(fams):,} resolution families")
        note(summary)
        if not rows:
            messagebox.showinfo(
                "Nothing left over" if unmatched else "Nothing to propose",
                "Every dumped texture either carries a replacement already or "
                "has a candidate close enough to review under Auto-assign."
                if unmatched else
                "No source texture came close to any target.")
            return
        AutoAssignWindow(self, targets, self.txd_index, rows,
                         self.pack_dir, dump_dir, installed, fams,
                         colour=self.colour.get(),
                         mode="unmatched" if unmatched else "assign",
                         note=summary)

    def compare(self, hash_name, dist):
        """Open the side-by-side view for one candidate."""
        dump = self.match_dump or self._dump_dir()
        try:
            with Image.open(os.path.join(dump, hash_name + ".png")) as src:
                psp_image = src.convert("RGBA")
        except Exception as exc:
            messagebox.showerror("Could not open", str(exc))
            return

        src_image = self.texture.image()
        CompareWindow(
            self,
            src_image,
            f"{self.texture.name}   —   source texture\n"
            f"{src_image.width}×{src_image.height}  {self.texture.format_name}"
            f"   ·   {self.selected.label}",
            psp_image,
            f"{hash_name}   —   LCS (PSP dump)\n"
            f"{psp_image.width}×{psp_image.height}"
            f"   ·   match distance {dist:.3f}",
            mode=self.bg_mode.get(),
            on_install=lambda h=hash_name: self.install(h),
            colour=self.colour.get())

    def install(self, hash_name):
        """Write the selected texture into the pack. True when it happened."""
        if not self.selected or not self.pack_dir:
            return False
        dump_dir = self.match_dump or self._dump_dir()
        name = self.texture.name
        source = self.selected.label
        how = (" scaled to the PSP texture's brightness"
               if self.colour.get() else "")
        if not messagebox.askyesno(
                "Install replacement",
                f"Write {name} ({self.texture.width}\u00d7{self.texture.height}, "
                f"from {source})\ninto the pack as {hash_name}{how}?\n\n"
                f"This edits textures.ini in\n{self.pack_dir}"):
            return False
        pack = ("android" if txd.is_obb(self.selected.source) else
                lcsmatch.TxdIndex._pack_of(self._source_root(self.selected.source),
                                           self.selected.source))
        rel = lcsmatch.install_path(hash_name, pack, name, self.selected.container)
        texture = self.texture
        try:
            done, failed = lcsmatch.install_many(
                [(hash_name, lambda: texture.image(), rel,
                  f"<- {source}/{name} ({pack})")],
                self.pack_dir, dump_dir, colour=self.colour.get())
        except Exception:
            messagebox.showerror("Install failed", traceback.format_exc(limit=2))
            return False
        if failed:
            messagebox.showerror("Install failed", "\n".join(failed))
            return False
        self.status.configure(text=f"installed {hash_name} \u2190 {name}  ({done[hash_name]})")
        return True

    def _source_root(self, path):
        """The opened source folder a file sits under, for naming its pack."""
        here = os.path.normcase(os.path.abspath(path))
        for folder in self.folders:
            root = os.path.abspath(folder)
            if here.startswith(os.path.normcase(root) + os.sep):
                return root
        return os.path.dirname(os.path.abspath(path))


HERE = os.path.dirname(os.path.abspath(__file__))
WORKSPACE = os.path.dirname(os.path.dirname(HERE))
# The VCS fork's sources (PC upscale packs, Definitive Edition dumps) and the Android OBB.
DEFAULT_SOURCES = (
    os.path.join(os.path.dirname(WORKSPACE), "gtavcspc", "sources"),
    os.path.join(WORKSPACE, "com.rockstargames.gtalcs", "main.17.com.rockstargames.gtalcs.obb"),
)


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("folders", nargs="*",
                    help="source folders of .txd files, IMG archives or images, and .obb files")
    ap.add_argument("--pack", help="PPSSPP texture pack folder (holds textures.ini)")
    args = ap.parse_args()

    folders = args.folders or [g for g in DEFAULT_SOURCES if os.path.exists(g)]
    pack = args.pack
    if not pack:
        guess = os.path.join(os.path.dirname(HERE), "memstick", "PSP", "TEXTURES", "ULUS10041")
        pack = guess if os.path.isdir(guess) else None

    Browser(folders, pack).mainloop()


if __name__ == "__main__":
    main()
