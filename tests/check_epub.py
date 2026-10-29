#!/usr/bin/env python3
"""Checks .epub files the way a small e-reader parser would, plus epubcheck
when available (EPUBCHECK=/path/to/epubcheck.jar or /opt/epubcheck-*/)."""
import glob, os, subprocess, sys, zipfile
import xml.etree.ElementTree as ET

def check(path):
    z = zipfile.ZipFile(path)
    infos = z.infolist()
    assert infos[0].filename == "mimetype", "mimetype must be first"
    assert infos[0].compress_type == zipfile.ZIP_STORED
    assert z.read("mimetype") == b"application/epub+zip"
    raw = open(path, "rb").read(80)
    assert raw[30:38] == b"mimetype" and raw[38:58] == b"application/epub+zip", "mimetype header"
    assert z.testzip() is None, "CRC error"
    for i in infos:
        if i.filename.endswith((".xml", ".xhtml", ".opf", ".ncx")):
            ET.fromstring(z.read(i.filename))  # well-formed XML
    ns = {"o": "http://www.idpf.org/2007/opf"}
    opf = ET.fromstring(z.read("OEBPS/content.opf"))
    items = {i.get("id"): i.get("href") for i in opf.find("o:manifest", ns)}
    spine = [items[r.get("idref")] for r in opf.find("o:spine", ns)]
    for h in items.values():
        z.getinfo("OEBPS/" + h)
    return len(spine)

jar = os.environ.get("EPUBCHECK") or next(iter(sorted(glob.glob("/opt/epubcheck-*/epubcheck.jar"))), None)
bad = 0
for p in sys.argv[1:]:
    try:
        n = check(p)
        msg = "%d chapter(s)" % n
        if jar:
            r = subprocess.run(["java", "-jar", jar, "-q", p], capture_output=True, text=True)
            if r.returncode:
                raise AssertionError("epubcheck:\n" + r.stdout + r.stderr)
            msg += ", epubcheck ok"
        print("%s: %s" % (p, msg))
    except Exception as e:
        bad += 1
        print("%s: FAIL %s" % (p, e))
sys.exit(1 if bad else 0)
