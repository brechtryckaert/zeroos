#!/usr/bin/env python3
"""Generate a small sample EPUB so the reader has something to open.

Usage: python3 tools/make_sample_epub.py assets/books/zeroos-guide.epub
"""
import sys
import uuid
import zipfile

TITLE = "ZeroOS Reader Guide"
AUTHOR = "ZeroOS"

CHAPTERS = [
    ("Welcome", """
<p>This little book ships with ZeroOS so the reader has something to open the
first time you start it. It is also a handy test file: it has chapters, a table
of contents, emphasis, a list and a long run of plain text, which are the things
a reflowing reader has to get right.</p>
<p>The screen on the CardputerZero is only 320 by 170 pixels. That is small, but
it is sharp, and a page of 9 or 10 short lines turns out to be perfectly pleasant
to read as long as turning the page is <em>instant</em>. The reader renders the
next page in the background while you are still reading this one, so pressing a
key only has to copy pixels to the panel.</p>
<p>Your place in every book is remembered. Close a book, open another, come back
next week: you land on the same paragraph, even if you changed the font size in
between, because positions are stored as content bookmarks rather than page
numbers.</p>
"""),
    ("Controls", """
<p>Everything is driven from the keyboard. The defaults are:</p>
<ul>
<li><b>Right, Down, Space or Enter</b> &#8212; next page</li>
<li><b>Left, Up or Backspace</b> &#8212; previous page</li>
<li><b>+ and &#8722;</b> &#8212; larger or smaller text</li>
<li><b>T</b> &#8212; cycle the colour theme (paper, sepia, night)</li>
<li><b>C</b> &#8212; table of contents</li>
<li><b>Esc or Q</b> &#8212; back to the library</li>
</ul>
<p>In the library, use Up and Down to choose a book and Enter to open it. Esc
takes you back to the launcher. The bottom line of every page shows the chapter
and how far you are through the book.</p>
"""),
    ("Adding books", """
<p>Copy EPUB files into the <code>Books</code> folder on the microSD card. The
library scans the folder each time it opens, reads the title and author from
each book and sorts by the book you read most recently.</p>
<p>Because the reader is built on MuPDF, it also opens PDF, CBZ comic archives,
FB2 and plain XHTML files. PDFs do not reflow, so they are only comfortable on
this screen if they were made for small screens; EPUB is the format to prefer.</p>
<p>Images inside books are shown in greyscale and tinted to match the theme, so a
night-mode page does not suddenly flash a bright white illustration at you in a
dark room.</p>
"""),
    ("A longer chapter", """
<p>This chapter exists to give the page-turning code a decent amount of text to
chew on. Scroll through it to check that line breaks, justification and
hyphenation behave, and that the progress indicator at the bottom moves
smoothly.</p>
""" + "".join(
        f"<p>Paragraph {i}. A pocket computer is a curious thing to read on. It has "
        "none of the calm of an e-ink panel and none of the size of a tablet, yet it "
        "is always with you, which counts for more than either. Short lines are easy "
        "on the eye; the trick is a good typeface, generous line spacing and margins "
        "that are small without feeling cramped. Tune the font size with the plus "
        "and minus keys until a line holds roughly forty characters.</p>"
        for i in range(1, 25)
    )),
]

CSS = """
body { font-family: serif; }
h1 { font-size: 1.3em; margin: 0 0 0.5em 0; }
p { margin: 0; text-indent: 1.2em; text-align: justify; }
h1 + p { text-indent: 0; }
ul { margin: 0.3em 0 0.3em 1em; padding: 0; }
code { font-family: monospace; }
"""


def xhtml(title, body):
    return f"""<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>{title}</title><link rel="stylesheet" href="style.css"/></head>
<body><h1>{title}</h1>{body}</body></html>
"""


def main(out):
    book_id = f"urn:uuid:{uuid.uuid5(uuid.NAMESPACE_URL, 'zeroos-guide')}"
    manifest, spine, navpoints, navlis = [], [], [], []
    for i, (title, _) in enumerate(CHAPTERS, 1):
        manifest.append(f'<item id="c{i}" href="c{i}.xhtml" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="c{i}"/>')
        navpoints.append(
            f'<navPoint id="n{i}" playOrder="{i}"><navLabel><text>{title}</text></navLabel>'
            f'<content src="c{i}.xhtml"/></navPoint>')
        navlis.append(f'<li><a href="c{i}.xhtml">{title}</a></li>')

    opf = f"""<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
 <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
  <dc:identifier id="bookid">{book_id}</dc:identifier>
  <dc:title>{TITLE}</dc:title>
  <dc:creator>{AUTHOR}</dc:creator>
  <dc:language>en</dc:language>
  <meta property="dcterms:modified">2026-10-23T00:00:00Z</meta>
 </metadata>
 <manifest>
  <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
  <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
  <item id="css" href="style.css" media-type="text/css"/>
  {''.join(manifest)}
 </manifest>
 <spine toc="ncx">{''.join(spine)}</spine>
</package>
"""
    ncx = f"""<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
<head><meta name="dtb:uid" content="{book_id}"/></head>
<docTitle><text>{TITLE}</text></docTitle>
<navMap>{''.join(navpoints)}</navMap></ncx>
"""
    nav = f"""<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Contents</title></head>
<body><nav epub:type="toc"><ol>{''.join(navlis)}</ol></nav></body></html>
"""
    container = """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
 <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
"""
    with zipfile.ZipFile(out, "w") as z:
        # mimetype must be first and stored uncompressed
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", container, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", opf, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/toc.ncx", ncx, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/nav.xhtml", nav, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/style.css", CSS, compress_type=zipfile.ZIP_DEFLATED)
        for i, (title, body) in enumerate(CHAPTERS, 1):
            z.writestr(f"OEBPS/c{i}.xhtml", xhtml(title, body), compress_type=zipfile.ZIP_DEFLATED)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "zeroos-guide.epub")
