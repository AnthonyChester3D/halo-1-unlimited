"""library_manager.pyw: the map library's manager, on the owner's PC (docs/map_library.md). Double-click it (Windows:
Python from python.org, nothing else to install); it needs library.py and oxsign.py beside it.

The library's folder (chosen in Settings) holds:
    library.json     the library as it should be: each map's catalog fields, and its source folder (Halo Map
                     Porter's "Copy to Xbox maps\\<group>\\<map>"), kept relative to this folder where it can be
    published.oxlb   the catalog last published (signed), what the Xboxes see now
    zips\\            each map's zip, made from its source

Add maps (a map's folder, or a folder of them: HMP's "Crossplay"), Add a pack (a whole map pack's folder, made small:
its maps' folders and its .shared file; the Xbox puts it in maps\\<folder>\\ as a pack of its own), Remove (or Restore), Check for changes (each map
made again from its source: a map HMP made again shows Changed), then Publish: the catalog signed with the library
key (its passphrase asked, or remembered by Windows for this login), the new zips uploaded to the library
repository's release, the catalog put in place of the old one, and zips nothing lists any more deleted from the
release. The players' installers see it at once.

GitHub: a fine-grained personal access token of the library's account, for the library repository alone, with
"Contents: Read and write" (Settings). Windows keeps it encrypted to your login (DPAPI), as oxsign.py remembers a
passphrase. (H1U_LIBRARY_TEST_API=http://host:port: a stand-in for GitHub, for testing.)
"""
import base64, json, os, queue, struct, sys, threading, time, traceback, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import library  # noqa: E402
import oxsign   # noqa: E402

import tkinter as tk                                    # noqa: E402
from tkinter import filedialog, messagebox, simpledialog, ttk  # noqa: E402

DEFAULT_REPO = 'h1u-librarian/map-library'
DEFAULT_TAG = 'library'
CATALOG = 'catalog.oxlb'
TEST_API = os.environ.get('H1U_LIBRARY_TEST_API', '').rstrip('/')
API = TEST_API or 'https://api.github.com'
UPLOADS = TEST_API or 'https://uploads.github.com'
WEB = TEST_API or 'https://github.com'
# (the installer's list of library keys: in the repository, or a copy beside this)
LIBRARY_KEYS = next((p for p in (os.path.join(HERE, '..', 'installer', 'library_keys.txt'),
                                 os.path.join(HERE, 'library_keys.txt')) if os.path.exists(p)), '')
USER_AGENT = 'H1U-library-manager'


# ---- settings (this PC's: %LOCALAPPDATA%\Halo1Unlimited\library_manager.json) ----

def settings_path():
    return os.path.join(oxsign._settings_folder(), 'library_manager.json')


def load_settings():
    try:
        return json.load(open(settings_path(), encoding='utf-8'))
    except (OSError, ValueError):
        return {}


def save_settings(settings):
    os.makedirs(oxsign._settings_folder(), exist_ok=True)
    with open(settings_path(), 'w', encoding='utf-8') as f:
        json.dump(settings, f, indent=1)


def seal(text):
    """the token, kept so only this Windows login can read it (elsewhere, a test machine: only encoded)"""
    data = text.encode()
    if sys.platform == 'win32':
        return 'dpapi:' + base64.b64encode(oxsign._dpapi(data, True)).decode()
    return 'plain:' + base64.b64encode(data).decode()


def unseal(value):
    if not value:
        return ''
    kind, _, data = value.partition(':')
    data = base64.b64decode(data)
    return (oxsign._dpapi(data, False) if kind == 'dpapi' else data).decode()


# ---- the library (its folder) ----

class Library:
    def __init__(self, folder):
        self.folder = folder
        self.items = []          # (the library as it should be: catalog fields + 'source', 'removed')
        self.published = []      # (the published catalog's items)
        self.published_serial = 0
        self.load()

    def path(self, *parts):
        return os.path.join(self.folder, *parts)

    def load(self):
        try:
            data = json.load(open(self.path('library.json'), encoding='utf-8'))
            self.items = data.get('items', [])
        except (OSError, ValueError):
            self.items = []
        try:
            self.published_serial, self.published = library.read_catalog(self.path('published.oxlb'))
        except (OSError, SystemExit):
            self.published_serial, self.published = 0, []

    def save(self):
        os.makedirs(self.folder, exist_ok=True)
        temp = self.path('library.json.new')
        with open(temp, 'w', encoding='utf-8') as f:
            json.dump({'about': 'Halo 1 Unlimited map library (library_manager.pyw)', 'items': self.items}, f,
                      indent=1)
        os.replace(temp, self.path('library.json'))

    def source(self, item):
        """an item's source folder (kept relative to the library's folder)"""
        return os.path.normpath(os.path.join(self.folder, item.get('source', '').replace('/', os.sep)))

    def relative(self, folder):
        try:
            return os.path.relpath(folder, self.folder).replace(os.sep, '/')
        except ValueError:
            return folder.replace(os.sep, '/')          # (another drive)

    def state(self, item):
        published = {library.item_key(p): p for p in self.published}
        old = published.get(library.item_key(item))
        if item.get('removed'):
            return 'To remove' if old else None
        if not old:
            return 'New'
        return 'Published' if old['sha256'] == item['sha256'] else 'Changed'

    def rows(self):
        """(state, item) for each map: the library's, and the published ones it has no record of (taken over)"""
        mine = {library.item_key(i) for i in self.items}
        rows = [(self.state(i), i) for i in self.items if self.state(i)]
        rows += [('Published (no source)', p) for p in self.published if library.item_key(p) not in mine]
        rows.sort(key=lambda r: (r[1].get('group', '').lower(), r[1]['name'].lower(), r[1]['item'].lower()))
        return rows

    def add(self, folder, group, say, pack=False):
        """a map's folder (or each in a folder of them; pack: a whole pack's folder) made into its zip and put in the
        library"""
        made = 0
        for map_folder in [folder] if pack else library.map_folders(folder):
            item, data = library.make_item(map_folder, group, pack=pack)
            name = os.path.basename(os.path.normpath(map_folder))
            if item is None:
                say('skipped %s: %s' % (name, data))
                continue
            os.makedirs(self.path('zips'), exist_ok=True)
            zip_path = self.path('zips', item['file'])
            if not os.path.exists(zip_path):
                with open(zip_path + '.new', 'wb') as f:
                    f.write(data)
                os.replace(zip_path + '.new', zip_path)
            item['source'] = self.relative(map_folder)
            key = library.item_key(item)
            old = [i for i in self.items if library.item_key(i) == key]
            if old and old[0]['sha256'] == item['sha256'] and not old[0].get('removed'):
                say('%s (%s): unchanged' % (name, group))
            else:
                say('%s (%s): %s' % (name, group, 'made again' if old else 'added'))
            self.items = [i for i in self.items if library.item_key(i) != key] + [item]
            made += 1
        self.save()
        return made

    def check(self, say, progress):
        """every map made again from its source: what changed"""
        changed = missing = 0
        for n, item in enumerate(list(self.items)):
            progress(n, len(self.items))
            if item.get('removed'):
                continue
            if not item.get('source'):
                say('%s: no source folder recorded (add it again to have one)' % item['item'])
                continue
            source = self.source(item)
            if not os.path.isdir(source):
                say('%s: its source isn\'t there (%s)' % (item['item'], source))
                missing += 1
                continue
            fresh, data = library.make_item(source, item.get('group', ''), pack=item.get('kind') == 'pack')
            if fresh is None:
                say('%s: %s' % (item['item'], data))
                continue
            if fresh['sha256'] != item['sha256']:
                zip_path = self.path('zips', fresh['file'])
                with open(zip_path + '.new', 'wb') as f:
                    f.write(data)
                os.replace(zip_path + '.new', zip_path)
                fresh['source'] = item['source']
                self.items[self.items.index(item)] = fresh
                say('%s (%s): changed' % (item['item'], item.get('group', '')))
                changed += 1
        progress(1, 1)
        self.save()
        say('Checked %d maps: %d changed%s.' % (len(self.items), changed,
                                               ', %d sources missing' % missing if missing else ''))

    def wanted(self):
        """the catalog's items to publish: the library's, and published ones it has no record of (kept)"""
        mine = {library.item_key(i) for i in self.items}
        items = [{k: v for k, v in i.items() if k not in ('source', 'removed')} for i in self.items
                 if not i.get('removed')]
        items += [p for p in self.published if library.item_key(p) not in mine]
        library.sort_items(items)
        return items


# ---- GitHub ----

class GitHub:
    def __init__(self, repo, token):
        self.repo, self.token = repo, token

    def call(self, method, url, body=None, headers=None, raw=False):
        headers = dict(headers or {})
        headers.setdefault('Accept', 'application/vnd.github+json')
        headers['Authorization'] = 'Bearer ' + self.token
        headers['User-Agent'] = USER_AGENT
        headers['X-GitHub-Api-Version'] = '2022-11-28'
        data = body
        if isinstance(body, (dict, list)):
            data = json.dumps(body).encode()
            headers['Content-Type'] = 'application/json'
        request = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=600) as response:
                content = response.read()
        except urllib.error.HTTPError as e:
            text = e.read().decode('utf-8', 'replace')
            try:
                text = json.loads(text).get('message', text)
            except ValueError:
                pass
            raise GitHubError(e.code, text)
        if raw:
            return content
        return json.loads(content) if content else None

    def release(self, tag):
        """the release with this tag, or None"""
        try:
            return self.call('GET', '%s/repos/%s/releases/tags/%s' % (API, self.repo, tag))
        except GitHubError as e:
            if e.code == 404:
                return None
            raise

    def create_release(self, tag):
        try:
            return self.call('POST', '%s/repos/%s/releases' % (API, self.repo),
                             {'tag_name': tag, 'name': 'Map library', 'make_latest': 'true',
                              'body': 'The Halo 1 Unlimited map library: the installer\'s Map Library reads '
                                      'catalog.oxlb here. Managed with library_manager.pyw.'})
        except GitHubError as e:
            if e.code == 422:
                raise GitHubError(422, '%s (an empty repository can\'t have a release: on GitHub, add a README to '
                                       'it first)' % e.message)
            raise

    def assets(self, release):
        """all its files (name -> asset), page by page"""
        found, page = {}, 1
        while True:
            batch = self.call('GET', '%s/repos/%s/releases/%d/assets?per_page=100&page=%d' %
                              (API, self.repo, release['id'], page))
            for asset in batch:
                found[asset['name']] = asset
            if len(batch) < 100:
                return found
            page += 1

    def upload(self, release, name, path, content_type, progress):
        size = os.path.getsize(path)

        class Reader:
            def __init__(self):
                self.file, self.done, self.shown = open(path, 'rb'), 0, 0

            def read(self, n=-1):
                data = self.file.read(1 << 20 if n is None or n < 0 else n)
                self.done += len(data)
                if self.done - self.shown >= 1 << 20 or self.done == size:
                    self.shown = self.done
                    progress(self.done, size)
                return data

        reader = Reader()
        try:
            return self.call('POST', '%s/repos/%s/releases/%d/assets?name=%s' %
                             (UPLOADS, self.repo, release['id'], urllib.request.quote(name)), reader,
                             {'Content-Type': content_type, 'Content-Length': str(size)})
        finally:
            reader.file.close()

    def delete_asset(self, asset):
        self.call('DELETE', '%s/repos/%s/releases/assets/%d' % (API, self.repo, asset['id']))

    def make_latest(self, release):
        self.call('PATCH', '%s/repos/%s/releases/%d' % (API, self.repo, release['id']),
                  {'make_latest': 'true', 'draft': False, 'prerelease': False})


class GitHubError(Exception):
    def __init__(self, code, message):
        Exception.__init__(self, 'GitHub said %s: %s' % (code, message))
        self.code, self.message = code, message


def fetch_published(repo, tag):
    """the catalog players see now (the latest release's), or None when there's none"""
    url = '%s/%s/releases/latest/download/%s' % (WEB, repo, CATALOG)
    request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return response.read()
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


# ---- signing ----

def sign_catalog(data, seed):
    """the catalog with one signature by the seed's key (oxsign.py's catalog signing)"""
    public = oxsign.ed25519_public(seed)
    signature = oxsign.ed25519_sign(seed, oxsign.CATALOG_SIGNED_PREFIX + data)
    if not oxsign.ed25519_verify(public, oxsign.CATALOG_SIGNED_PREFIX + data, signature):
        raise RuntimeError('the signature didn\'t check out')
    return data + b'OXSG' + struct.pack('<I', 1) + oxsign.key_id(public) + signature


def trusted_by_installer(public):
    """whether the installer's library_keys.txt has this key (None: no library_keys.txt here)"""
    if not LIBRARY_KEYS:
        return None
    try:
        return any(k[2] == public for k in oxsign.load_trusted(LIBRARY_KEYS))
    except SystemExit:
        return None


# ---- the window ----

class App:
    def __init__(self, root):
        self.root = root
        self.settings = load_settings()
        self.library = None
        self.busy = False
        self.messages = queue.Queue()
        root.title('Halo 1 Unlimited map library')
        root.geometry('980x640')
        root.minsize(760, 480)

        top = ttk.Frame(root, padding=(8, 8, 8, 0))
        top.pack(fill='x')
        self.summary = ttk.Label(top, text='', font=('TkDefaultFont', 10, 'bold'))
        self.summary.pack(side='left')
        ttk.Button(top, text='Settings...', command=self.open_settings).pack(side='right')
        self.folder_label = ttk.Label(root, text='', foreground='#57606a', padding=(8, 2, 8, 0))
        self.folder_label.pack(fill='x')

        columns = ('state', 'group', 'folder', 'name', 'version', 'mb')
        middle = ttk.Frame(root, padding=8)
        middle.pack(fill='both', expand=True)
        self.tree = ttk.Treeview(middle, columns=columns, show='headings', selectmode='extended')
        for column, title, width, anchor in (('state', 'State', 130, 'w'), ('group', 'Group', 90, 'w'),
                                             ('folder', 'Folder', 220, 'w'), ('name', 'Name', 300, 'w'),
                                             ('version', 'Version', 60, 'center'), ('mb', 'MB', 70, 'e')):
            self.tree.heading(column, text=title)
            self.tree.column(column, width=width, anchor=anchor, stretch=column == 'name')
        scroll = ttk.Scrollbar(middle, orient='vertical', command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        for state, colour in (('New', '#1a7f37'), ('Changed', '#9a6700'), ('To remove', '#cf222e'),
                              ('Published (no source)', '#6e7781')):
            self.tree.tag_configure(state, foreground=colour)

        buttons = ttk.Frame(root, padding=(8, 0, 8, 8))
        buttons.pack(fill='x')
        self.buttons = []
        for text, command in (('Add maps...', self.add_maps), ('Add a pack...', self.add_pack), ('Remove', self.remove), ('Restore', self.restore),
                              ('Check for changes', self.check), ('Refresh from GitHub', self.refresh)):
            b = ttk.Button(buttons, text=text, command=command)
            b.pack(side='left', padx=(0, 6))
            self.buttons.append(b)
        self.publish_button = ttk.Button(buttons, text='Publish', command=self.publish)
        self.publish_button.pack(side='right')
        self.buttons.append(self.publish_button)

        bottom = ttk.Frame(root, padding=(8, 0, 8, 8))
        bottom.pack(fill='x')
        self.progress = ttk.Progressbar(bottom, mode='determinate', maximum=1000)
        self.progress.pack(fill='x')
        self.log = tk.Text(bottom, height=8, wrap='word', state='disabled', font=('Consolas', 9))
        self.log.pack(fill='x', pady=(6, 0))

        self.open_library()
        root.after(100, self.pump)
        if self.library:
            self.refresh()

    # (work in the background, its messages shown as they come)
    def pump(self):
        try:
            while True:
                kind, value = self.messages.get_nowait()
                if kind == 'say':
                    self.say_now(value)
                elif kind == 'progress':
                    self.progress['value'] = value
                elif kind == 'done':
                    self.busy = False
                    for b in self.buttons:
                        b.state(['!disabled'])
                    self.show()
                    if value:
                        value()
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def say(self, text):
        self.messages.put(('say', text))

    def say_now(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', time.strftime('%H:%M:%S  ') + text + '\n')
        self.log.see('end')
        self.log.configure(state='disabled')

    def set_progress(self, done, total):
        self.messages.put(('progress', 1000 * done / total if total else 0))

    def run(self, work, then=None):
        """work(), in the background (one thing at a time)"""
        if self.busy:
            return
        self.busy = True
        for b in self.buttons:
            b.state(['disabled'])

        def go():
            after = then
            try:
                result = work()
                if callable(result):
                    after = result
            except SystemExit as e:
                self.say('Stopped: %s' % e.code)
            except (GitHubError, urllib.error.URLError, OSError) as e:
                self.say('Stopped: %s' % e)
            except Exception as e:
                self.say('Stopped: %s' % e)
                self.say(traceback.format_exc().strip().splitlines()[-1])
            self.messages.put(('done', after))
        threading.Thread(target=go, daemon=True).start()

    def open_library(self):
        folder = self.settings.get('folder')
        if not folder:
            self.folder_label.configure(text='No library folder chosen: Settings...')
            self.library = None
        else:
            self.library = Library(folder)
            self.folder_label.configure(text='%s  ->  %s, release "%s"' % (
                folder, self.settings.get('repo', DEFAULT_REPO), self.settings.get('tag', DEFAULT_TAG)))
        self.show()

    def show(self):
        self.tree.delete(*self.tree.get_children())
        if not self.library:
            self.summary.configure(text='')
            return
        counts = {}
        for state, item in self.library.rows():
            counts[state] = counts.get(state, 0) + 1
            self.tree.insert('', 'end', iid='%s|%s' % library.item_key(item), tags=(state,), values=(
                state, item.get('group', ''), item['item'], item['name'], item.get('version', ''),
                '%.1f' % (int(item['size']) / 1e6)))
        pending = sum(n for s, n in counts.items() if s in ('New', 'Changed', 'To remove'))
        self.summary.configure(text='%d maps, %d published (serial %d)%s' % (
            sum(n for s, n in counts.items() if s != 'To remove'), len(self.library.published),
            self.library.published_serial, ', %d to publish' % pending if pending else ', nothing to publish'))

    def need_library(self):
        if not self.library:
            messagebox.showinfo('Map library', 'Choose the library\'s folder first (Settings).')
            return False
        return not self.busy

    def selected(self):
        keys = set(tuple(i.split('|', 1)) for i in self.tree.selection())
        return [i for i in self.library.items if library.item_key(i) in keys]

    # ---- the buttons ----

    def add_pack(self):
        if not self.need_library():
            return
        folder = filedialog.askdirectory(title='A whole map pack\'s folder (its maps\' folders and its .shared file)',
                                         initialdir=self.settings.get('last_add') or self.library.folder)
        if not folder:
            return
        folder = os.path.normpath(folder)
        self.settings['last_add'] = os.path.dirname(folder)
        save_settings(self.settings)
        problem = library.pack_problem(folder)
        if problem:
            messagebox.showinfo('Add a pack', '%s: %s.' % (os.path.basename(folder), problem))
            return
        group = simpledialog.askstring('Add a pack', '%s: %d maps. Its group (for example Classic packs):' % (
            os.path.basename(folder), len(library.map_folders(folder))), initialvalue='Packs', parent=self.root)
        if group is None:
            return
        self.say('Adding the pack %s...' % folder)
        self.run(lambda: self.say('%d made.' % self.library.add(folder, group.strip(), self.say, pack=True)))

    def add_maps(self):
        if not self.need_library():
            return
        folder = filedialog.askdirectory(title='A map\'s folder, or a folder of them (HMP\'s Crossplay or Xbox)',
                                         initialdir=self.settings.get('last_add') or self.library.folder)
        if not folder:
            return
        folder = os.path.normpath(folder)
        self.settings['last_add'] = os.path.dirname(folder)
        save_settings(self.settings)
        maps = library.map_folders(folder)
        if not maps:
            messagebox.showinfo('Add maps', 'No map in %s (a map\'s folder has its .map in it).' % folder)
            return
        single = maps == [folder]
        default = os.path.basename(os.path.dirname(folder) if single else folder)
        group = simpledialog.askstring('Add maps', '%d map%s. Their group (HMP\'s output folder: Crossplay or Xbox):'
                                       % (len(maps), '' if len(maps) == 1 else 's'), initialvalue=default,
                                       parent=self.root)
        if group is None:
            return
        self.say('Adding %d map%s from %s...' % (len(maps), '' if len(maps) == 1 else 's', folder))
        self.run(lambda: self.say('%d made.' % self.library.add(folder, group.strip(), self.say)))

    def remove(self):
        if not self.need_library():
            return
        chosen = self.selected()
        keys = set(tuple(i.split('|', 1)) for i in self.tree.selection())
        orphans = [p for p in self.library.published if library.item_key(p) in keys and
                   library.item_key(p) not in {library.item_key(i) for i in self.library.items}]
        for item in chosen:
            if self.library.state(item) == 'New':
                self.library.items.remove(item)
            else:
                item['removed'] = True
        for p in orphans:
            self.library.items.append(dict(p, removed=True, source=''))
        self.library.save()
        self.show()

    def restore(self):
        if not self.need_library():
            return
        for item in self.selected():
            item.pop('removed', None)
        self.library.save()
        self.show()

    def check(self):
        if not self.need_library():
            return
        self.say('Making every map again from its source...')
        self.run(lambda: self.library.check(self.say, self.set_progress))

    def refresh(self):
        if not self.library or self.busy:
            return
        repo, tag = self.settings.get('repo', DEFAULT_REPO), self.settings.get('tag', DEFAULT_TAG)

        def work():
            data = fetch_published(repo, tag)
            if data is None:
                self.say('Nothing published yet on %s.' % repo)
                return
            serial, items = library.parse_catalog(data)
            size = struct.unpack_from('<I', data, 8)[0]
            signed = data[12 + size:12 + size + 4] == b'OXSG'
            with open(self.library.path('published.oxlb'), 'wb') as f:
                f.write(data)
            self.library.load()
            self.say('Published: %d maps, serial %d, %s.' % (len(items), serial, 'signed' if signed else 'NOT signed'))
        self.say('Looking at what\'s published...')
        self.run(work)

    def passphrase_seed(self, key):
        """the key's seed: remembered by Windows, else its passphrase asked (in the window)"""
        seed = oxsign._remembered_seed(key)
        if seed:
            return seed
        for attempt in range(3):
            answer = PassphraseDialog(self.root, key.name).result
            if answer is None:
                return None
            passphrase, remember = answer
            seed = key.unseal(passphrase)
            if seed:
                if remember and sys.platform == 'win32':
                    try:
                        with open(oxsign._remembered_path(key), 'wb') as f:
                            f.write(oxsign._dpapi(seed, True))
                    except OSError as e:
                        messagebox.showwarning('Signing', 'Couldn\'t remember it: %s' % e)
                return seed
            messagebox.showerror('Signing', 'Wrong passphrase.')
        return None

    def publish(self):
        if not self.need_library():
            return
        lib = self.library
        wanted = lib.wanted()
        rows = lib.rows()
        pending = [r for r in rows if r[0] in ('New', 'Changed', 'To remove')]
        if not pending and lib.published:
            messagebox.showinfo('Publish', 'Nothing to publish: the library is as published.')
            return
        token = unseal(self.settings.get('token', ''))
        key_path = self.settings.get('key')
        if not token or not key_path or not os.path.exists(key_path):
            messagebox.showinfo('Publish', 'Settings first: the library key (library.oxkey) and a GitHub token.')
            return
        try:
            key = oxsign.KeyFile(key_path)
        except SystemExit as e:
            messagebox.showerror('Publish', str(e.code))
            return
        trusted = trusted_by_installer(key.public)
        if trusted is False and not messagebox.askyesno(
                'Publish', 'The installer\'s library_keys.txt doesn\'t have the key %s: Xboxes won\'t take a catalog '
                           'signed with it. Publish anyway?' % key.name):
            return
        missing = [i['file'] for i in wanted if not os.path.exists(lib.path('zips', i['file'])) and
                   library.item_key(i) in {library.item_key(x) for x in lib.items}]
        if missing:
            messagebox.showerror('Publish', 'Some zips aren\'t in the library\'s zips folder (Check for changes makes '
                                            'them again): %s' % ', '.join(missing[:5]))
            return
        counts = {}
        for state, _ in pending:
            counts[state] = counts.get(state, 0) + 1
        if not messagebox.askyesno('Publish', 'Publish %d maps (%s) as serial %d to %s?' % (
                len(wanted), ', '.join('%d %s' % (n, s.lower()) for s, n in sorted(counts.items())) or 'first time',
                lib.published_serial + 1, self.settings.get('repo', DEFAULT_REPO))):
            return
        seed = self.passphrase_seed(key)
        if not seed:
            return
        repo, tag = self.settings.get('repo', DEFAULT_REPO), self.settings.get('tag', DEFAULT_TAG)
        serial = lib.published_serial + 1
        catalog = sign_catalog(library.catalog_bytes(serial, wanted), seed)

        def work():
            hub = GitHub(repo, token)
            release = hub.release(tag)
            if release is None:
                self.say('Making the release "%s"...' % tag)
                release = hub.create_release(tag)
            assets = hub.assets(release)
            needed = {i['file'] for i in wanted}
            to_upload = [f for f in sorted(needed) if f not in assets]
            total = sum(os.path.getsize(lib.path('zips', f)) for f in to_upload)
            done_before = [0]
            self.say('%d zips to upload (%.0f MB), %d there already.' % (len(to_upload), total / 1e6,
                                                                        len(needed) - len(to_upload)))
            for n, name in enumerate(to_upload, 1):
                self.say('Uploading %s (%d of %d)...' % (name, n, len(to_upload)))
                hub.upload(release, name, lib.path('zips', name), 'application/zip',
                           lambda done, size: self.set_progress(done_before[0] + done, total))
                done_before[0] += os.path.getsize(lib.path('zips', name))
            # (the catalog last: the zips it lists are all there by then)
            staged = lib.path('published.oxlb.new')
            with open(staged, 'wb') as f:
                f.write(catalog)
            if CATALOG in assets:
                hub.delete_asset(assets[CATALOG])
            self.say('Putting the catalog (serial %d) in place...' % serial)
            hub.upload(release, CATALOG, staged, 'application/octet-stream', lambda done, size: None)
            os.replace(staged, lib.path('published.oxlb'))
            hub.make_latest(release)
            # (then the zips nothing lists any more)
            stale = [a for name, a in assets.items() if name.endswith('.zip') and name not in needed]
            for asset in stale:
                self.say('Deleting %s (not listed any more)...' % asset['name'])
                hub.delete_asset(asset)
            lib.items = [i for i in lib.items if not i.get('removed')]
            lib.save()
            lib.load()
            # (what players get now, checked)
            seen = fetch_published(repo, tag)
            self.say('Published: %d maps, serial %d. %s' % (
                len(wanted), serial, 'The release gives back exactly this catalog.' if seen == catalog else
                'The release\'s catalog doesn\'t read back the same yet (GitHub may take a moment): Refresh '
                'from GitHub to check.'))
            self.set_progress(1, 1)
        self.say('Publishing serial %d...' % serial)
        self.run(work)

    def open_settings(self):
        if self.busy:
            return
        dialog = SettingsDialog(self.root, self.settings)
        if dialog.result:
            self.settings = dialog.result
            save_settings(self.settings)
            self.open_library()


class PassphraseDialog(simpledialog.Dialog):
    def __init__(self, parent, name):
        self.name = name
        simpledialog.Dialog.__init__(self, parent, 'Sign the catalog')

    def body(self, frame):
        ttk.Label(frame, text='Passphrase for the key "%s":' % self.name).grid(row=0, column=0, sticky='w')
        self.entry = ttk.Entry(frame, show='*', width=40)
        self.entry.grid(row=1, column=0, pady=4)
        self.remember = tk.BooleanVar(value=False)
        if sys.platform == 'win32':
            ttk.Checkbutton(frame, text='Remember it on this PC (Windows keeps it encrypted to your login)',
                            variable=self.remember).grid(row=2, column=0, sticky='w')
        return self.entry

    def apply(self):
        self.result = (self.entry.get(), self.remember.get())


class SettingsDialog(simpledialog.Dialog):
    def __init__(self, parent, settings):
        self.settings = dict(settings)
        simpledialog.Dialog.__init__(self, parent, 'Settings')

    def body(self, frame):
        self.values = {}
        rows = (('folder', 'Library folder', 'folder'), ('key', 'Library key (library.oxkey)', 'file'),
                ('repo', 'GitHub repository', None), ('tag', 'Release tag', None))
        defaults = {'repo': DEFAULT_REPO, 'tag': DEFAULT_TAG}
        for n, (name, title, browse) in enumerate(rows):
            ttk.Label(frame, text=title).grid(row=n, column=0, sticky='w', pady=2)
            var = tk.StringVar(value=self.settings.get(name, defaults.get(name, '')))
            ttk.Entry(frame, textvariable=var, width=60).grid(row=n, column=1, pady=2)
            if browse:
                ttk.Button(frame, text='...', width=3, command=lambda v=var, b=browse: self.browse(v, b)).grid(
                    row=n, column=2, padx=2)
            self.values[name] = var
        n = len(rows)
        ttk.Label(frame, text='GitHub token').grid(row=n, column=0, sticky='w', pady=2)
        self.token = tk.StringVar(value='')
        ttk.Entry(frame, textvariable=self.token, width=60, show='*').grid(row=n, column=1, pady=2)
        ttk.Label(frame, text=('A token is saved (type a new one to replace it).' if self.settings.get('token') else
                               'None yet: the library account\'s fine-grained token, this repository only, '
                               '"Contents: Read and write".'), wraplength=480).grid(row=n + 1, column=1, sticky='w')

    def browse(self, var, kind):
        chosen = (filedialog.askdirectory(parent=self, initialdir=var.get() or None) if kind == 'folder' else
                  filedialog.askopenfilename(parent=self, filetypes=[('Signing key', '*.oxkey'), ('All', '*')]))
        if chosen:
            var.set(os.path.normpath(chosen))

    def apply(self):
        result = dict(self.settings)
        for name, var in self.values.items():
            result[name] = var.get().strip()
        if self.token.get().strip():
            result['token'] = seal(self.token.get().strip())
        self.result = result


def main():
    root = tk.Tk()
    try:
        ttk.Style().theme_use('vista' if sys.platform == 'win32' else 'clam')
    except tk.TclError:
        pass
    App(root)
    root.mainloop()


if __name__ == '__main__':
    main()
