# render_previews.py — regenerates the three theme previews (doc/webui/theme-*.png) from the
# REAL web UI rather than from a hand-built mockup: index.html is loaded as-is, its WebSocket
# is stubbed out, and the page's OWN render functions are fed one sample telemetry frame plus
# the head of the current parameter table. That is the point of doing it this way — a preview
# built by hand drifts (these three spent a month showing the pre-reversal kart, caster at the
# rear, and a low-voltage cutoff that no longer exists). This one cannot: it IS the page.
#
# Run from the repository root, with chromium and Pillow available:
#   . .venv-schem/bin/activate && python doc/webui/render_previews.py
#
# The theme CSS is read back out of themes.md, so the doc and the previews cannot disagree.
import re, shutil, subprocess, tempfile
from PIL import Image

SRC = 'firmware/main/assets'
OUT = tempfile.mkdtemp(prefix='kart-ui-preview-')
THEMES = {'tactical': 'Tactical / mil-spec', 'racing': 'Racing HUD', 'cyberpunk': 'Cyberpunk neon'}

# ── theme CSS blocks: read them from themes.md so doc and previews cannot drift ──
md = open('doc/webui/themes.md').read()
blocks = {}
for m in re.finditer(r'^## (.+?)$(.*?)```css\n(.*?)```', md, re.S | re.M):
    blocks[m.group(1).split('—')[0].strip().lower()] = m.group(3)

sample_status = """{
 state:2, fault:0, faults:0, speed_ms:0.86, rpm_l:905, rpm_r:898,
 fwd:0.60, turn:0.25, out_l:0.62, out_r:-0.30, brake_mode:0, arming:true, btn_start:false,
 pad_conn:true, pad_batt:87, pad_x:0.25, pad_y:0.60, pad_cx:0.25, pad_cy:0.60,
 pad_zl:0, pad_zr:0, pad_rx2:0, pad_ry2:0, pad_btns:0, pad_age_ms:12
}"""

# The first rows of the real PARAMS table (config_params.cpp), in display order.
sample_params = """[
 {name:'speed_limit_ms',desc:'Speed limit (m/s)',cat:'Speed & power',fval:3.3,fmin:0.3,fmax:7},
 {name:'rev_speed_ms',  desc:'Reverse limit (m/s)',cat:'Speed & power',fval:1.0,fmin:0.3,fmax:3},
 {name:'duty_cap',      desc:'PWM cap (0-1)',   cat:'Speed & power',fval:1.0,fmin:0.05,fmax:1},
 {name:'thr_deadzone',  desc:'Stick deadzone',  cat:'Gamepad',      fval:0.06,fmin:0,fmax:0.3},
 {name:'turn_gain',     desc:'Turn gain (0-1)', cat:'Gamepad',      fval:1.0,fmin:0,fmax:1},
 {name:'mix_type',      desc:'Mixing type (0/1/2)',cat:'Drive feel',ival:1,imin:0,imax:2},
 {name:'turn_limit_en', desc:'Rollover protection (0/1)',cat:'Rollover protection',bval:true,bmin:false,bmax:true},
 {name:'use_encoders',  desc:'Use encoders (0/1)',cat:'Behavior',   bval:true,bmin:false,bmax:true}
]"""

EPILOGUE = """
<script>
// Static preview: no kart behind the page, so feed its own render functions one frame.
const fa = document.getElementById('faults'), cf = document.getElementById('cfg');
fa.hidden = false; cf.hidden = false;
fa.parentNode.insertBefore(fa, cf);          // Dashboard first, then Configuration
document.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t.dataset.tab === 'faults'));
buildForm(%s);
showStatus(%s);
document.getElementById('status').textContent = 'Preview — sample data, no kart connected';
document.documentElement.setAttribute('data-h', document.body.scrollHeight + 40);
</script>
""" % (sample_params, sample_status)

html = open(f'{SRC}/index.html').read()
# This machine has no emoji font (headless CI box), and tofu boxes would be the most visible
# thing in a preview whose whole point is the skin. Swap the handful of emoji the rendered
# panels use for plain glyphs; the real page in a real browser shows the emoji.
for a, b in {'\U0001F3CE': '\u25C9', '\uFE0F': '', '\U0001F6A8': '\u25B2', '\u2705': '\u2713',
             '\U0001F512': '\u26BF', '\U0001F3AE': '\u25CF', '\U0001F6D1': '\u25CF',
             '\U0001F50B': '\u25AE', '\u26A1': '\u2726', '\U0001F4CC': '\u25AA',
             '\U0001F4A1': '\u2726', '\U0001F393': '\u25AA', '\U0001F527': '\u25AA',
             '\U0001F517': '\u25AA', '\U0001F5D1': '\u2717', '\U0001F4BE': '\u25AA',
             '\U0001F579': '\u25CF', '\U0001F9F2': '\u25AA', '\U0001FAB5': '\u25AA',
             '\U0001F6DE': '\u25CB', '\u2699': '\u25AA'}.items():
    html = html.replace(a, b)
html = html.replace('href="/style.css"', 'href="style.css"')
html = html.replace('src="/chart.js"', 'src="chart.min.js"').replace('src="/pb.js"', 'src="pb.min.js"')
# Stub the WebSocket before the page script runs: connect() must not throw, and nothing must poll.
html = html.replace('</head>', """<script>
window.WebSocket = function(){ this.send=function(){}; this.close=function(){};
  Object.defineProperty(this,'binaryType',{set:function(){},get:function(){return 'arraybuffer';}}); };
</script>
</head>""")
html = html.replace('</body>', EPILOGUE + '</body>')

for name in ('style.css', 'chart.min.js', 'pb.min.js'):
    shutil.copy(f'{SRC}/{name}', f'{OUT}/{name}')

for key, title in THEMES.items():
    css = blocks[title.lower()]
    page = html.replace('</head>', '<style>\n%s\n</style>\n</head>' % css)
    path = f'{OUT}/mock_{key}.html'
    open(path, 'w').write(page)
    png = f'{OUT}/shot_{key}.png'
    W, SCALE = 640, 1.5          # .wrap is 600 px wide; 1.5x keeps the text crisp
    def shot(height, dump=False):
        args = ['chromium', '--headless=new', '--disable-gpu', '--hide-scrollbars',
                f'--force-device-scale-factor={SCALE}', f'--window-size={W},{height}',
                '--virtual-time-budget=4000']
        args += ['--dump-dom'] if dump else [f'--screenshot={png}']
        return subprocess.run(args + [f'file://{path}'], check=True, capture_output=True).stdout
    # First pass: ask the page itself how tall it is, then shoot exactly that.
    dom = shot(3000, dump=True).decode('utf8', 'replace')
    h = int(re.search(r'data-h="(\d+)"', dom).group(1))
    shot(h)
    Image.open(png).convert('RGB').save(f'doc/webui/theme-{key}.png')
    print('theme-%s.png' % key, Image.open('doc/webui/theme-%s.png' % key).size)

shutil.rmtree(OUT, ignore_errors=True)
