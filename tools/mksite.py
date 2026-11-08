#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把仓库里的 Markdown 文档合成一个可离线查询的单页 HTML。

    python tools/mksite.py            # 生成 docs/site/index.html
    python tools/mksite.py --check    # 只校验产物是否与源文档一致（供 test.sh 用）

设计约束：
  * 零第三方依赖。环境里没有 markdown / mistune / pandoc，所以自带一个
    只覆盖本项目文档语法的轻量渲染器（标题 / 表格 / 围栏代码 / 列表 /
    引用 / 分隔线 / 行内代码·粗体·斜体·链接 / 显式锚点）。
  * 零外部资源。CSS、JS、字体、搜索索引全部内联，双击 index.html 即可用，
    断网、file:// 协议下都不降级。
  * 产物里写一份源文件 sha256 清单（#zx-build）。文档改了却没重跑本脚本，
    --check 立刻失败——离线文档站最怕的就是"查到的是旧内容"。
  * 行尾统一 LF；源文件 CRLF / LF 混用都在读入时归一化，不影响哈希
    （哈希基于归一化后的字节，跨平台一致）。
"""

import hashlib
import html
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "site" / "index.html"

# (id, 侧栏名, 源路径)。顺序即侧栏顺序，第一个是默认打开的文档。
SOURCES = [
    ("usage", "zxc 使用说明", "docs/zxc-usage.md"),
    ("manual", "Zx 语言手册", "docs/manual.md"),
    ("guide", "入门指南", "docs/guide.md"),
    ("errors", "错误码参考", "docs/errors.md"),
    ("readme", "README", "README.md"),
    ("spec", "语言规范", "spec/zx-spec.md"),
    ("decisions", "设计决策", "spec/decisions.md"),
    ("bootstrap", "自举说明", "spec/bootstrap.md"),
]

# 文档之间的 .md 链接转成站内跳转（data-doc / data-frag），而不是让浏览器
# 在 file:// 下打开一份 Markdown 纯文本。
LINK_DOCS = {}
for _id, _title, _path in SOURCES:
    LINK_DOCS[_path.lower()] = _id
    LINK_DOCS[Path(_path).name.lower()] = _id

KEYWORDS = set("""
const let var fn pub extern module struct enum union return if else while for
switch case default break continue defer inline comptime test true false null
void bool i8 i16 i32 i64 u8 u16 u32 u64 usize isize f32 f64 type anytype
import export typedef static int char float double long short unsigned signed
include define ifdef endif pragma sizeof sizeof nullptr NULL auto self
""".split())

TOKEN = re.compile(
    r'("(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|#[^\n]*'
    r'|\b\d+(?:\.\d+)?\b|\b[A-Za-z_][A-Za-z_0-9]*\b)',
    re.S,
)


def esc(s):
    return html.escape(s, quote=False)


def qattr(s):
    return html.escape(s, quote=True)


def highlight(code):
    """代码块着色。先分词再逐段 escape，避免 span 插进转义后的字符串里。"""
    out = []
    for part in TOKEN.split(code):
        if not part:
            continue
        cls = ""
        if part.startswith("//") or part.startswith("/*") or part.startswith("#"):
            cls = "c"
        elif part[0] in "\"'":
            cls = "s"
        elif part[0].isdigit():
            cls = "n"
        elif part in KEYWORDS:
            cls = "k"
        e = esc(part)
        out.append('<span class="t-%s">%s</span>' % (cls, e) if cls else e)
    return "".join(out)


def strip_md(s):
    """搜索索引用的纯文本：去掉标记，保留内容。"""
    s = re.sub(r'`([^`]*)`', r'\1', s)
    s = re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', s)
    s = re.sub(r'<[^>]+>', '', s)
    s = re.sub(r'\*\*|__|(?<!\w)\*|(?<!\w)_', '', s)
    return s


def rewrite_href(u):
    """返回 (href, 额外属性)。站内 .md 链接交给 JS 切文档。"""
    if u.startswith(("http://", "https://", "mailto:")):
        return qattr(u), ' target="_blank" rel="noopener" class="xref out"'
    if u.startswith("#"):
        return qattr(u), ' class="xref local"'
    base, _, frag = u.partition("#")
    base = base.strip()
    key = base.lower().lstrip("./").replace("\\", "/")
    while key.startswith("../"):
        key = key[3:]
    did = LINK_DOCS.get(key) or LINK_DOCS.get(Path(key).name.lower())
    if did:
        attrs = ' class="xref" data-doc="%s"' % did
        if frag:
            attrs += ' data-frag="%s"' % qattr(frag)
        return "#", attrs
    return qattr(u), ' class="xref out"'


def inline(s):
    """行内渲染。行内代码先占位保护，避免被链接/强调规则误伤。"""
    codes = []

    def stash(m):
        codes.append(m.group(1))
        return "\x01%d\x01" % (len(codes) - 1)

    s = re.sub(r'`([^`]+)`', stash, s)
    s = esc(s)
    s = re.sub(
        r'\[([^\]]+)\]\(([^)\s]+)(?:\s+"[^"]*")?\)',
        lambda m: '<a href="%s"%s>%s</a>' % (
            rewrite_href(m.group(2))[0], rewrite_href(m.group(2))[1], m.group(1)),
        s,
    )
    s = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', s)
    s = re.sub(r'(?<!\*)\*([^*\n]+)\*(?!\*)', r'<em>\1</em>', s)
    s = re.sub(
        "\x01(\\d+)\x01",
        lambda m: "<code>%s</code>" % esc(codes[int(m.group(1))]),
        s,
    )
    return s


def split_row(s):
    s = s.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|"):
        s = s[:-1]
    parts = re.split(r"(?<!\\)\|", s)
    return [p.strip().replace("\\|", "|") for p in parts]


def is_table(lines, i):
    if i + 1 >= len(lines):
        return False
    a, b = lines[i].lstrip(), lines[i + 1].lstrip()
    if not a.startswith("|") or not b.startswith("|"):
        return False
    cells = [c.strip() for c in b.strip().strip("|").split("|")]
    return bool(cells) and all(
        c == "" or re.fullmatch(r":?-{2,}:?", c) for c in cells
    )


class Renderer:
    def __init__(self, docid, docidx):
        self.docid = docid
        self.docidx = docidx
        self.n = 0
        self.para = []
        self.out = []
        self.toc = []
        self.blocks = []       # [bid, heading, text]
        self.heading = ""

    def bid(self):
        self.n += 1
        return "b%d_%d" % (self.docidx, self.n)

    def index(self, bid, text):
        t = re.sub(r"\s+", " ", strip_md(text)).strip()
        if t:
            self.blocks.append([bid, self.heading, t])

    def flush(self):
        if not self.para:
            return
        text = "\n".join(self.para).strip()
        self.para = []
        if not text:
            return
        text = re.sub(r" {2}\n", "\x00", text).replace("\n", " ")
        bid = self.bid()
        body = inline(text).replace("\x00", "<br>")
        self.out.append('<p id="%s">%s</p>' % (bid, body))
        self.index(bid, text)

    def render_list(self, items):
        # 缩进只用来表达层级（class d1..d3），全部平铺成 <li>：文档里嵌套
        # 列表不多，平铺能保证 HTML 一定合法，浏览器不会补出诡异的闭合。
        base = min(ind for ind, _, _ in items)
        res = ['<ul class="lst">']
        for ind, marker, txt in items:
            bid = self.bid()
            depth = min(3, max(0, (ind - base) // 2))
            cls = (' d%d' % depth) if depth else ""
            numeric = bool(re.fullmatch(r"\d+[.)]", marker))
            attr = ' data-n="%s"' % qattr(marker) if numeric else ""
            res.append('<li id="%s" class="li%s%s"%s>%s</li>' % (
                bid, cls, " num" if numeric else "", attr, inline(txt)))
            self.index(bid, txt)
        res.append("</ul>")
        self.out.append("".join(res))

    def render(self, text):
        lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        i = 0
        nlines = len(lines)
        while i < nlines:
            ln = lines[i]

            # 围栏代码块
            if ln.lstrip().startswith("```"):
                self.flush()
                lang = ln.strip()[3:].strip().split(" ")[0]
                i += 1
                code = []
                while i < nlines and not lines[i].lstrip().startswith("```"):
                    code.append(lines[i])
                    i += 1
                i += 1
                raw = "\n".join(code)
                bid = self.bid()
                self.out.append(
                    '<pre id="%s" class="code" data-lang="%s"><code>%s</code></pre>'
                    % (bid, qattr(lang), highlight(raw))
                )
                self.index(bid, raw)
                continue

            # 标题
            m = re.match(r"^(#{1,6})\s+(.*)$", ln)
            if m:
                self.flush()
                level = len(m.group(1))
                title = m.group(2).strip()
                self.n += 1                      # 保证连续标题的 hid 也不撞
                slug = re.sub(r"[^\w\u4e00-\u9fff-]", "-", title.lower())
                slug = re.sub(r"-+", "-", slug).strip("-")[:60]
                hid = "h-%s-%d-%s" % (self.docid, self.n, slug)
                self.out.append('<h%d id="%s">%s</h%d>' % (
                    level, qattr(hid), inline(title), level))
                self.toc.append([level, strip_md(title), hid])
                self.heading = strip_md(title)
                # 索引必须指向 DOM 里真实存在的 id：标题身上只有 hid，
                # 再另发一个 bid 会让搜索结果点不动（静态校验会抓到）。
                self.index(hid, title)
                i += 1
                continue

            # errors.md 的显式锚点 <a id="e0304"></a>
            m = re.match(r'^\s*<a\s+id="([^"]+)"\s*>\s*</a>\s*$', ln)
            if m:
                self.flush()
                self.out.append('<span id="%s" class="anchor"></span>' % qattr(m.group(1)))
                i += 1
                continue

            # 分隔线
            if re.fullmatch(r"\s*(?:-{3,}|\*{3,}|_{3,})\s*", ln):
                self.flush()
                self.out.append("<hr>")
                i += 1
                continue

            # 表格
            if is_table(lines, i):
                self.flush()
                header = split_row(lines[i])
                aligns = split_row(lines[i + 1])
                i += 2
                rows = []
                while i < nlines and lines[i].lstrip().startswith("|") and lines[i].strip():
                    rows.append(split_row(lines[i]))
                    i += 1
                t = ['<div class="tw"><table><thead><tr>']
                for k, cell in enumerate(header):
                    a = aligns[k] if k < len(aligns) else ""
                    style = ""
                    if a.startswith(":") and a.endswith(":"):
                        style = ' style="text-align:center"'
                    elif a.endswith(":"):
                        style = ' style="text-align:right"'
                    t.append("<th%s>%s</th>" % (style, inline(cell)))
                t.append("</tr></thead><tbody>")
                for row in rows:
                    bid = self.bid()
                    t.append('<tr id="%s">' % bid)
                    for cell in row:
                        t.append("<td>%s</td>" % inline(cell))
                    t.append("</tr>")
                    self.index(bid, " | ".join(row))
                t.append("</tbody></table></div>")
                self.out.append("".join(t))
                continue

            # 列表（含续行）
            if re.match(r"^\s*(?:[-*+]|\d+[.)])\s+", ln):
                self.flush()
                items = []
                while i < nlines:
                    m2 = re.match(r"^(\s*)([-*+]|\d+[.)])\s+(.*)$", lines[i])
                    if not m2:
                        if (lines[i].strip() and items
                                and re.match(r"^\s{2,}\S", lines[i])):
                            ind, marker, txt = items[-1]
                            items[-1] = (ind, marker, txt + " " + lines[i].strip())
                            i += 1
                            continue
                        break
                    items.append((len(m2.group(1).replace("\t", "    ")),
                                  m2.group(2), m2.group(3)))
                    i += 1
                self.render_list(items)
                continue

            # 引用
            if re.match(r"^\s*>", ln):
                self.flush()
                quote = []
                while i < nlines and re.match(r"^\s*>", lines[i]):
                    quote.append(re.sub(r"^\s*>\s?", "", lines[i]))
                    i += 1
                bid = self.bid()
                raw = "\n".join(quote)
                self.out.append('<blockquote id="%s">%s</blockquote>' % (
                    bid, inline(raw.replace("\n", " "))))
                self.index(bid, raw)
                continue

            # 缩进代码块（4 空格 / Tab）
            if re.match(r"^(?: {4}|\t)\S", ln):
                self.flush()
                code = []
                while i < nlines and (re.match(r"^(?: {4}|\t)", lines[i]) or not lines[i].strip()):
                    code.append(re.sub(r"^(?: {4}|\t)", "", lines[i]))
                    i += 1
                raw = "\n".join(code).rstrip()
                bid = self.bid()
                self.out.append('<pre id="%s" class="code"><code>%s</code></pre>' % (
                    bid, highlight(raw)))
                self.index(bid, raw)
                continue

            if not ln.strip():
                self.flush()
                i += 1
                continue

            self.para.append(ln)
            i += 1

        self.flush()
        return "".join(self.out)


CSS = r"""
*{box-sizing:border-box}
:root{
  --bg:#fbfaf7; --fg:#26231e; --side:#f3f0e9; --line:#e2ddd2;
  --accent:#a2571f; --accent-soft:#f3e6d6; --muted:#777065;
  --code-bg:#f4f1ea; --code-fg:#2f2b25; --mark:#ffe08a; --card:#fff;
  --shadow:0 1px 3px rgba(0,0,0,.06);
}
html[data-theme=dark]{
  --bg:#161513; --fg:#e7e2d8; --side:#1d1c19; --line:#33302a;
  --accent:#e0a458; --accent-soft:#33291c; --muted:#9a9386;
  --code-bg:#201f1b; --code-fg:#e0dccf; --mark:#7a5a12; --card:#1b1a17;
  --shadow:0 1px 3px rgba(0,0,0,.4);
}
html,body{height:100%;margin:0}
body{
  background:var(--bg);color:var(--fg);
  font:15px/1.75 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC",
       "Microsoft YaHei","Hiragino Sans GB",sans-serif;
  -webkit-font-smoothing:antialiased;
}
#app{display:flex;height:100vh;overflow:hidden}
#side{
  width:310px;flex:none;background:var(--side);border-right:1px solid var(--line);
  display:flex;flex-direction:column;min-height:0;
}
.brand{padding:16px 18px 10px;display:flex;align-items:center;gap:10px}
.brand b{font-size:16px;letter-spacing:.5px}
.brand small{color:var(--muted);font-size:11px;margin-left:auto}
#searchwrap{padding:8px 14px 12px;position:relative}
#q{
  width:100%;padding:9px 30px 9px 12px;border:1px solid var(--line);
  border-radius:8px;background:var(--card);color:var(--fg);font-size:13px;outline:none;
}
#q:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-soft)}
#qk{
  position:absolute;right:24px;top:17px;color:var(--muted);font-size:11px;
  border:1px solid var(--line);border-radius:4px;padding:0 5px;pointer-events:none;
}
#doclist{
  display:flex;flex-wrap:wrap;gap:6px;padding:2px 14px 12px;
  border-bottom:1px solid var(--line);
}
.chip{
  font-size:12px;padding:4px 9px;border:1px solid var(--line);border-radius:20px;
  background:transparent;color:var(--muted);cursor:pointer;white-space:nowrap;
}
.chip:hover{border-color:var(--accent);color:var(--accent)}
.chip.on{background:var(--accent);border-color:var(--accent);color:#fff}
html[data-theme=dark] .chip.on{color:#201a12}
#pane{flex:1;overflow-y:auto;padding:10px 10px 40px;min-height:0}
#pane h4{
  margin:14px 8px 6px;font-size:11px;letter-spacing:1px;color:var(--muted);
  text-transform:uppercase;font-weight:600;
}
.toc a{
  display:block;color:var(--fg);text-decoration:none;font-size:12.5px;
  padding:3px 8px;border-radius:5px;line-height:1.5;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap;
}
.toc a:hover{background:var(--accent-soft);color:var(--accent)}
.toc a.lv1{font-weight:700;margin-top:8px}
.toc a.lv2{padding-left:18px}
.toc a.lv3{padding-left:30px;color:var(--muted)}
.toc a.lv4,.toc a.lv5,.toc a.lv6{padding-left:42px;color:var(--muted);font-size:12px}
.res{
  display:block;text-decoration:none;color:var(--fg);padding:8px 10px;
  border-bottom:1px solid var(--line);font-size:12.5px;
}
.res:hover{background:var(--accent-soft)}
.res .meta{color:var(--accent);font-size:11px;display:block;margin-bottom:2px}
.res .snip{color:var(--muted);line-height:1.5;display:block;
  display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden}
mark{background:var(--mark);color:inherit;border-radius:2px;padding:0 1px}
.empty{padding:18px 10px;color:var(--muted);font-size:12.5px}
#main{flex:1;overflow-y:auto;min-width:0;scroll-behavior:smooth}
#topbar{
  position:sticky;top:0;z-index:5;background:var(--bg);
  border-bottom:1px solid var(--line);padding:9px 34px;display:flex;
  align-items:center;gap:12px;font-size:12px;color:var(--muted);
}
#topbar .crumb{color:var(--accent);font-weight:600}
#topbar .spacer{flex:1}
#theme{
  border:1px solid var(--line);background:var(--card);color:var(--fg);
  border-radius:6px;padding:4px 10px;cursor:pointer;font-size:12px;
}
#content{max-width:980px;padding:26px 40px 140px;margin:0 auto}
article{display:none}
article.on{display:block}
h1,h2,h3,h4,h5,h6{line-height:1.35;scroll-margin-top:60px;font-weight:700}
h1{font-size:30px;margin:12px 0 22px;padding-bottom:12px;border-bottom:2px solid var(--accent)}
h2{font-size:22px;margin:38px 0 14px;padding-bottom:7px;border-bottom:1px solid var(--line)}
h3{font-size:17px;margin:26px 0 10px}
h4{font-size:15px;margin:20px 0 8px}
h5,h6{font-size:14px;margin:16px 0 6px;color:var(--muted)}
p{margin:10px 0}
a.xref{color:var(--accent);text-decoration:none;border-bottom:1px dotted var(--accent)}
a.xref:hover{background:var(--accent-soft)}
a.xref.out{cursor:pointer}
code{
  font:12.5px/1.6 ui-monospace,SFMono-Regular,Consolas,"Liberation Mono",monospace;
  background:var(--code-bg);color:var(--code-fg);padding:1px 5px;border-radius:4px;
}
pre.code{
  background:var(--code-bg);color:var(--code-fg);border:1px solid var(--line);
  border-radius:8px;padding:14px 16px;overflow-x:auto;margin:14px 0;
  font:12.5px/1.65 ui-monospace,SFMono-Regular,Consolas,"Liberation Mono",monospace;
  white-space:pre;tab-size:4;
}
pre.code code{background:none;padding:0;font:inherit}
.t-k{color:#a8332a;font-weight:600}
.t-s{color:#3f7d4e}
.t-c{color:#8a857a;font-style:italic}
.t-n{color:#8a5a20}
html[data-theme=dark] .t-k{color:#e88b7d}
html[data-theme=dark] .t-s{color:#8fc99b}
html[data-theme=dark] .t-c{color:#7d786e}
html[data-theme=dark] .t-n{color:#e0b06a}
.tw{overflow-x:auto;margin:14px 0}
table{border-collapse:collapse;width:100%;font-size:13.5px}
th,td{border:1px solid var(--line);padding:7px 11px;text-align:left;vertical-align:top}
th{background:var(--side);font-weight:600}
tr:nth-child(even) td{background:var(--card)}
.lst{margin:10px 0;padding-left:22px}
.lst li{margin:4px 0}
.lst li.num{list-style:none;position:relative}
.lst li.num::before{content:attr(data-n);position:absolute;left:-22px;color:var(--accent)}
.lst li.d1{margin-left:18px}.lst li.d2{margin-left:36px}.lst li.d3{margin-left:54px}
blockquote{
  margin:14px 0;padding:8px 16px;border-left:3px solid var(--accent);
  background:var(--accent-soft);border-radius:0 6px 6px 0;color:var(--fg);
}
hr{border:0;border-top:1px solid var(--line);margin:26px 0}
.anchor{display:block;height:0;scroll-margin-top:64px}
.flash{animation:fl 1.5s ease-out}
@keyframes fl{0%,35%{background:var(--accent-soft)}100%{background:transparent}}
#foot{padding:16px 18px;border-top:1px solid var(--line);color:var(--muted);font-size:11px}
@media(max-width:820px){
  #app{flex-direction:column}
  #side{width:100%;height:42%;border-right:0;border-bottom:1px solid var(--line)}
  #content{padding:20px 18px 100px}
  #topbar{padding:8px 18px}
}
"""

JS = r"""
(function(){
'use strict';
var $ = function(s){return document.querySelector(s)};
var docs = DOCS, idx = IDX, heads = HEADS;
var current = 'usage';
var pane = $('#pane'), q = $('#q'), content = $('#content');

/* 主题：记住选择，首次跟随系统 */
var saved = null;
try { saved = localStorage.getItem('zxdoc-theme'); } catch(e){}
var dark = saved ? saved === 'dark'
    : window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
function applyTheme(){
  document.documentElement.setAttribute('data-theme', dark ? 'dark':'light');
  $('#theme').textContent = dark ? '\u2600 浅色' : '\u263E 深色';
}
applyTheme();
$('#theme').onclick = function(){
  dark = !dark; applyTheme();
  try { localStorage.setItem('zxdoc-theme', dark ? 'dark':'light'); } catch(e){}
};

/* 文档切换 */
function showDoc(id, noHash){
  if(!document.getElementById('doc-'+id)) id = 'usage';
  current = id;
  var arts = content.querySelectorAll('article');
  for(var i=0;i<arts.length;i++) arts[i].classList.toggle('on', arts[i].id === 'doc-'+id);
  var chips = document.querySelectorAll('.chip');
  for(var j=0;j<chips.length;j++) chips[j].classList.toggle('on', chips[j].dataset.id === id);
  var d = docs.filter(function(x){return x.id===id})[0];
  if(d) $('#crumb').textContent = d.title;
  buildToc(id);
  $('#main').scrollTop = 0;
  if(!noHash){ try { history.replaceState(null,'','#'+id); } catch(e){} }
}
window.showDoc = showDoc;

function buildToc(id){
  if(q.value.trim()) return;              /* 搜索态下目录让位给结果 */
  var d = docs.filter(function(x){return x.id===id})[0];
  var s = '<h4>本页目录</h4><div class="toc">';
  if(!d || !d.toc.length) s += '<div class="empty">该文档没有标题</div>';
  else for(var i=0;i<d.toc.length;i++){
    var t = d.toc[i];
    s += '<a class="lv'+t[0]+'" href="#'+t[2]+'" data-hid="'+t[2]+'">'
       + escapeHtml(t[1]) + '</a>';
  }
  s += '</div>';
  pane.innerHTML = s;
}

function escapeHtml(s){
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')
    .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

/* 全文搜索：多词 AND，大小写不敏感，最多 120 条 */
function search(raw){
  var words = raw.trim().toLowerCase().split(/\s+/).filter(Boolean);
  if(!words.length){ buildToc(current); return; }
  var hits = [];
  for(var i=0;i<idx.length && hits.length<120;i++){
    var bid=idx[i][0], d=idx[i][1], h=idx[i][2], t=idx[i][3];
    var low=t.toLowerCase(), pos=-1, ok=true;
    for(var w=0;w<words.length;w++){
      var p=low.indexOf(words[w]);
      if(p<0){ ok=false; break; }
      if(w===0) pos=p;
    }
    if(ok) hits.push([bid,d,h,t,pos,words[0]]);
  }
  var s='<h4>'+hits.length+(hits.length>=120?'+':'')+' 条结果 · 全部文档</h4>';
  if(!hits.length) s+='<div class="empty">没有命中。换个关键词，或只搜一个词。</div>';
  for(var k=0;k<hits.length;k++){
    var r=hits[k], text=r[3], p=r[4], wlen=r[5].length;
    var from=Math.max(0,p-42), to=Math.min(text.length,p+wlen+90);
    var snip=escapeHtml((from?'…':'')+text.slice(from,to)+(to<text.length?'…':''));
    var re=new RegExp('('+words.map(function(x){
      return x.replace(/[.*+?^${}()|[\]\\]/g,'\\$&');
    }).join('|')+')','gi');
    snip=snip.replace(re,'<mark>$1</mark>');
    var dn=docs[r[1]] ? docs[r[1]].title : '';
    s+='<a class="res" href="#" data-bid="'+r[0]+'" data-doc="'+docs[r[1]].id+'">'
      +'<span class="meta">'+escapeHtml(dn)+' · '+escapeHtml(heads[r[2]]||'')
      +'</span><span class="snip">'+snip+'</span></a>';
  }
  pane.innerHTML=s;
}

function jump(bid, docid){
  if(docid && docid!==current){ showDoc(docid); }
  var el=document.getElementById(bid);
  if(!el) return;
  el.scrollIntoView({behavior:'smooth',block:'center'});
  el.classList.remove('flash');
  void el.offsetWidth;
  el.classList.add('flash');
}

pane.addEventListener('click', function(ev){
  var a=ev.target.closest ? ev.target.closest('a') : null;
  if(!a) return;
  if(a.dataset.bid){
    ev.preventDefault(); jump(a.dataset.bid, a.dataset.doc); return;
  }
  if(a.dataset.hid){
    ev.preventDefault(); jump(a.dataset.hid, null); return;
  }
});

/* 站内 .md 链接 → 切文档；#frag 若对应本页显式锚点则继续定位 */
content.addEventListener('click', function(ev){
  var a=ev.target.closest ? ev.target.closest('a.xref') : null;
  if(!a || !a.dataset.doc) return;
  ev.preventDefault();
  var did=a.dataset.doc, frag=a.dataset.frag;
  showDoc(did);
  if(frag){
    var el=document.getElementById(frag) || document.getElementById('doc-'+did)
      .querySelector('[id$="-'+frag+'"]');
    if(el) setTimeout(function(){ el.scrollIntoView({block:'center'}); },30);
  }
});

/* chips */
document.getElementById('doclist').addEventListener('click', function(ev){
  var c=ev.target.closest ? ev.target.closest('.chip') : null;
  if(c){ q.value=''; showDoc(c.dataset.id); }
});

var timer=null;
q.addEventListener('input', function(){
  clearTimeout(timer);
  timer=setTimeout(function(){ search(q.value); },110);
});
q.addEventListener('keydown', function(ev){
  if(ev.key==='Escape'){ q.value=''; search(''); q.blur(); }
  if(ev.key==='Enter'){
    var f=pane.querySelector('.res');
    if(f){ ev.preventDefault(); jump(f.dataset.bid,f.dataset.doc); }
  }
});
document.addEventListener('keydown', function(ev){
  if((ev.ctrlKey||ev.metaKey) && ev.key.toLowerCase()==='k'){
    ev.preventDefault(); q.focus(); q.select();
  }
});

/* 初始：hash 优先，其次默认 usage */
var ini=(location.hash||'').slice(1);
if(docs.filter(function(x){return x.id===ini}).length) showDoc(ini,true);
else if(ini){ showDoc('usage',true);
  var t=document.getElementById(ini);
  if(t) setTimeout(function(){t.scrollIntoView({block:'center'})},40);
} else showDoc('usage',true);
})();
"""


def build():
    docs_meta, articles, index, heads = [], [], [], []
    head_ids = {}
    buildinfo = {}
    for di, (did, title, rel) in enumerate(SOURCES):
        raw = (ROOT / rel).read_bytes()
        text = raw.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")
        buildinfo[rel] = hashlib.sha256(text.encode("utf-8")).hexdigest()
        r = Renderer(did, di)
        body = r.render(text)
        articles.append('<article id="doc-%s" data-id="%s">%s</article>' % (did, did, body))
        docs_meta.append({"id": did, "title": title, "toc": r.toc})
        for bid, heading, t in r.blocks:
            if heading not in head_ids:
                head_ids[heading] = len(heads)
                heads.append(heading)
            index.append([bid, di, head_ids[heading], t])

    chips = "".join(
        '<button class="chip%s" data-id="%s">%s</button>' %
        (" on" if i == 0 else "", d, esc(t))
        for i, (d, t, _) in enumerate(SOURCES)
    )

    def jsdata(obj):
        return json.dumps(obj, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")

    page = """<!DOCTYPE html>
<html lang="zh-CN" data-theme="light">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Zx 离线文档</title>
<meta name="description" content="Zx 语言与 zxc 工具链的离线文档合订本：命令、语言手册、入门、错误码、规范、设计决策。">
<style>%s</style>
</head>
<body>
<div id="app">
  <aside id="side">
    <div class="brand"><b>Zx 离线文档</b><small>全文可搜 · 无需联网</small></div>
    <div id="searchwrap"><input id="q" placeholder="搜索全部文档…  Ctrl+K" autocomplete="off" spellcheck="false"><span id="qk">⌘K</span></div>
    <div id="doclist">%s</div>
    <div id="pane"></div>
    <div id="foot">单文件离线文档 · 源文档改了请重跑 <code>python tools/mksite.py</code></div>
  </aside>
  <main id="main">
    <div id="topbar"><span>当前：</span><span class="crumb" id="crumb"></span><span class="spacer"></span><span id="count"></span><button id="theme">◑</button></div>
    <div id="content">%s</div>
  </main>
</div>
<script id="zx-build" type="application/json">%s</script>
<script>
var DOCS=%s, IDX=%s, HEADS=%s;
%s
</script>
</body>
</html>
""" % (CSS, chips, "".join(articles), jsdata(buildinfo),
       jsdata(docs_meta), jsdata(index), jsdata(heads), JS)

    page = page.replace(
        '<span id="count"></span>',
        '<span id="count">%d 篇 · %d 个可检索段落</span>' % (len(SOURCES), len(index)),
    )
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(page, encoding="utf-8", newline="\n")
    print("wrote %s  (%d bytes, %d docs, %d searchable blocks)" %
          (OUT.relative_to(ROOT), len(page.encode("utf-8")), len(SOURCES), len(index)))
    return 0


def check():
    if not OUT.exists():
        print("missing %s (run: python tools/mksite.py)" % OUT, file=sys.stderr)
        return 1
    page = OUT.read_text(encoding="utf-8")
    m = re.search(
        r'<script id="zx-build" type="application/json">(.*?)</script>', page, re.S)
    if not m:
        print("no build manifest in %s" % OUT, file=sys.stderr)
        return 1
    old = json.loads(m.group(1))
    bad = []
    for _, _, rel in SOURCES:
        text = (ROOT / rel).read_bytes().decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")
        now = hashlib.sha256(text.encode("utf-8")).hexdigest()
        if old.get(rel) != now:
            bad.append(rel)
    if bad:
        print("stale offline docs, rerun `python tools/mksite.py`: " + ", ".join(bad),
              file=sys.stderr)
        return 1
    print("offline docs up to date (%d sources)" % len(SOURCES))
    return 0


if __name__ == "__main__":
    sys.exit(check() if "--check" in sys.argv[1:] else build())
