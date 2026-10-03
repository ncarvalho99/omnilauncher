/* Catalog data is untrusted: it is published by an ingest pipeline that accepts
   third-party submissions. Every node here is built with DOM APIs and every
   path is validated before it reaches an element. Nothing from the JSON is ever
   parsed as HTML, and no value is interpolated into a URL without checking. */
(() => {
  'use strict';

  const output = document.querySelector('#out');
  const totalsList = document.querySelector('#totals');
  const motionToggle = document.querySelector('#motion-toggle');
  const motionLabel = document.querySelector('#motion-label');
  const searchInput = document.querySelector('#q');
  const chips = [...document.querySelectorAll('.chip')];
  const footSource = document.querySelector('#foot-src');

  // Previews are the content of this catalogue, not decoration. The OS reduced
  // motion setting used to turn every theme into a still with no visible way
  // back, so the choice is explicit here and remembered per visitor. The CSS
  // still honours the OS setting for the interface's own movement.
  let motionEnabled = true;
  try { motionEnabled = localStorage.getItem('switchu-theme-motion') !== 'off'; } catch { /* storage optional */ }

  let themes = [];
  let activeFilter = 'todos';
  let query = '';
  let loopTimer = 0;

  const element = (name, className, text) => {
    const node = document.createElement(name);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = String(text);
    return node;
  };
  const integer = value => (Number.isFinite(Number(value)) ? Math.max(0, Math.floor(Number(value))) : 0);
  const megabytes = value => {
    const bytes = integer(value);
    if (bytes >= 1073741824) return `${(bytes / 1073741824).toFixed(1)} GB`;
    return bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : `${Math.round(bytes / 1024)} KB`;
  };
  const decimals = value => integer(value).toLocaleString('pt-BR');

  // Only same-origin, relative paths are allowed from JSON. URL() also rejects
  // traversal after normalisation, protocol-relative URLs, and encoded surprises.
  const safePath = value => {
    if (typeof value !== 'string' || value.length === 0 || value.length > 240 || /[\\\0]/.test(value)) return null;
    try {
      const base = new URL(`${location.origin}${location.pathname.replace(/[^/]*$/, '')}`);
      const url = new URL(value, base);
      if (url.origin !== location.origin || !url.pathname.startsWith(base.pathname) || url.search || url.hash) return null;
      return `${url.pathname}${url.search}`;
    } catch { return null; }
  };
  const asset = (tag, path, options = {}) => {
    const safe = safePath(path);
    if (!safe) return null;
    const node = element(tag);
    node.src = safe;
    Object.assign(node, options);
    return node;
  };
  const flag = (text, state = '') => element('span', `flag ${state}`.trim(), text);
  const clear = className => { output.replaceChildren(); output.className = className; };

  const loopSeconds = theme => {
    const frames = integer(theme.frameCount);
    const fps = Number(theme.fps);
    return frames > 0 && Number.isFinite(fps) && fps > 0 ? frames / fps : 0;
  };

  /* ------------------------------------------------------------- chrome */

  function renderTotals() {
    const totals = themes.reduce((value, theme) => ({
      frames: value.frames + integer(theme.frameCount),
      bytes: value.bytes + integer(theme.packageBytes || theme.bytes),
      videoBytes: value.videoBytes + integer(theme.packageVideoBytes || theme.packageBytes || theme.bytes),
      animated: value.animated + (theme.animated ? 1 : 0),
    }), { frames: 0, bytes: 0, videoBytes: 0, animated: 0 });

    const entry = (label, value) => {
      const box = element('div');
      box.append(element('dt', '', label), element('dd', '', value));
      return box;
    };
    totalsList.replaceChildren(
      entry('Temas', decimals(themes.length)),
      entry('Animados', decimals(totals.animated)),
      entry('Total OmniLaunch', megabytes(totals.videoBytes)),
      entry('Total SwitchU', megabytes(totals.bytes)),
    );
    footSource.textContent = `Catálogo servido para SwitchU (index.json) e OmniLaunch (omnilaunch.json).`;
  }

  function renderMotionToggle() {
    motionToggle.setAttribute('aria-pressed', String(motionEnabled));
    motionLabel.textContent = motionEnabled ? 'Animações ligadas' : 'Animações desligadas';
  }

  /* -------------------------------------------------------------- media */

  function mediaFor(theme, stage = false) {
    const videoPath = motionEnabled ? safePath(theme.previewVideo) : null;
    if (videoPath) {
      const media = asset('video', videoPath, {
        muted: true, loop: true, playsInline: true,
        preload: stage ? 'metadata' : 'none',
      });
      if (media) {
        const poster = safePath(theme.previewPoster);
        if (poster) media.poster = poster;
        if (stage) {
          media.autoplay = true;
        } else {
          media.dataset.src = videoPath;
          media.removeAttribute('src');
        }
        return media;
      }
    }
    // Still fallback: with motion off, or when a theme ships no clip. The
    // previous build read an undeclared variable here, which threw under strict
    // mode and left the catalogue blank whenever animations were turned off.
    const still = theme.previewPoster || theme.preview || theme.cover;
    return asset('img', still, {
      loading: stage ? 'eager' : 'lazy',
      decoding: 'async',
      alt: '',
    });
  }

  function observeVideos() {
    const videos = output.querySelectorAll('video[data-src]');
    if (!videos.length) return;
    const activate = video => {
      if (!video.src) {
        video.src = video.dataset.src;
        video.play().catch(() => { /* autoplay may be refused; poster remains */ });
      }
    };
    if (!('IntersectionObserver' in window)) { [...videos].slice(0, 6).forEach(activate); return; }
    const observer = new IntersectionObserver(
      entries => entries.forEach(({ target, isIntersecting }) => (isIntersecting ? activate(target) : target.pause())),
      { rootMargin: '250px' },
    );
    videos.forEach(video => observer.observe(video));
  }

  // One timer drives every loop bar. Per-video timeupdate events fire at the
  // browser's convenience and would be both noisier and less even than this.
  function startLoopMeters() {
    cancelAnimationFrame(loopTimer);
    const step = () => {
      output.querySelectorAll('video[data-loop]').forEach(video => {
        const bar = video.parentElement && video.parentElement.querySelector('.loop i');
        if (!bar) return;
        const span = video.duration;
        if (!Number.isFinite(span) || span <= 0) return;
        bar.style.width = `${Math.min(100, (video.currentTime / span) * 100)}%`;
      });
      loopTimer = requestAnimationFrame(step);
    };
    loopTimer = requestAnimationFrame(step);
  }

  /* --------------------------------------------------------------- grid */

  function visibleThemes() {
    const needle = query.trim().toLowerCase();
    return themes.filter(theme => {
      if (activeFilter === 'animados' && !theme.animated) return false;
      if (activeFilter === 'trilha' && !integer(theme.music)) return false;
      if (activeFilter === 'sem-licenca' && theme.license) return false;
      if (!needle) return true;
      return `${theme.name || ''} ${theme.author || ''}`.toLowerCase().includes(needle);
    });
  }

  function buildTile(theme) {
    const tile = element('button', 'tile');
    tile.type = 'button';
    tile.addEventListener('click', () => { location.hash = encodeURIComponent(String(theme.id || '')); });

    const frame = element('div', 'frame');
    const media = mediaFor(theme);
    if (media) {
      if (media.tagName === 'VIDEO') media.dataset.loop = '1';
      frame.append(media);
    }

    const seconds = loopSeconds(theme);
    const badge = element('span', theme.animated ? 'badge' : 'badge static',
      theme.animated && seconds ? `Laço ${seconds.toFixed(1)}s` : (theme.animated ? 'Animado' : 'Estático'));
    frame.append(badge);

    if (theme.animated) {
      const loop = element('div', 'loop');
      loop.append(element('i'));
      frame.append(loop);
    }

    const meta = element('div', 'meta');
    meta.append(
      element('span', 'nm', theme.name || 'Tema sem nome'),
      element('span', 'sub', `${theme.author || 'Autor desconhecido'} · ${megabytes(theme.bytes)}`),
    );

    const flags = element('span', 'flags');
    if (theme.animated) flags.append(flag(`${decimals(theme.frameCount)} quadros`, 'on'));
    if (integer(theme.music)) flags.append(flag('trilha', 'on'));
    flags.append(theme.license ? flag(theme.license) : flag('sem licença', 'warn'));
    meta.append(flags);

    tile.append(frame, meta);
    return tile;
  }

  function renderGrid() {
    const list = visibleThemes();
    if (!themes.length) { clear('state'); output.textContent = 'Catálogo vazio.'; return; }
    if (!list.length) {
      clear('state');
      output.textContent = 'Nenhum tema corresponde a esse filtro. Limpe a busca para ver os 61 temas.';
      return;
    }
    clear('grid');
    list.forEach(theme => output.append(buildTile(theme)));
    observeVideos();
    startLoopMeters();
  }

  /* ------------------------------------------------------------- detail */

  const row = (label, value) => {
    const line = element('div');
    line.append(element('dt', '', label), element('i', 'dots'), element('dd', '', value));
    return line;
  };

  function renderDetail(theme, manifest = {}) {
    clear('');
    const back = element('button', 'back', '← Voltar ao catálogo');
    back.type = 'button';
    back.addEventListener('click', () => { location.hash = ''; });
    output.append(back);

    const detail = element('div', 'detail');
    const visual = element('div');
    const stage = element('div', 'stage');
    const media = mediaFor(theme, true);
    if (media) {
      if (media.tagName === 'VIDEO' && theme.animated) {
        media.dataset.loop = '1';
        const loop = element('div', 'loop');
        loop.append(element('i'));
        stage.append(media, loop);
      } else {
        stage.append(media);
      }
    }
    visual.append(stage);

    const base = String(theme.path || '').replace(/^\/+|\/+$/g, '');
    const screenshots = Array.isArray(manifest.preview?.screenshots)
      ? manifest.preview.screenshots.map(name => safePath(`${base}/${name}`)).filter(Boolean)
      : [];
    if (screenshots.length) {
      const strip = element('div', 'strip');
      screenshots.forEach(path => {
        const button = element('button');
        button.type = 'button';
        const thumb = asset('img', path, { loading: 'lazy', decoding: 'async', alt: '' });
        if (!thumb) return;
        button.append(thumb);
        button.addEventListener('click', () => {
          const full = asset('img', path, { alt: '' });
          if (full) stage.replaceChildren(full);
        });
        strip.append(button);
      });
      visual.append(strip);
    }

    const info = element('div');
    info.append(
      element('h2', 'title', theme.name || 'Tema sem nome'),
      element('p', 'sub', theme.author || 'Autor desconhecido'),
    );

    const seconds = loopSeconds(theme);
    const spec = element('dl', 'spec');
    spec.append(
      row('identificador', theme.id || '-'),
      row('versão', theme.version || '-'),
      row('modo', manifest.theme?.mode || '-'),
      row('fundo', theme.animated
        ? `${decimals(theme.frameCount)} quadros @ ${theme.fps || '-'} fps`
        : 'imagem estática'),
      row('duração do laço', seconds ? `${seconds.toFixed(2)} s` : '-'),
      row('trilha', integer(theme.music) ? `${integer(theme.music)} faixa(s)` : 'nenhuma'),
      row('download (OmniLaunch MP4)', theme.packageVideoBytes ? megabytes(theme.packageVideoBytes) : (theme.packageBytes ? megabytes(theme.packageBytes) : megabytes(theme.bytes))),
      row('espaço console (OmniLaunch)', theme.installedVideoBytes ? megabytes(theme.installedVideoBytes) : (theme.installedBytes ? megabytes(theme.installedBytes) : '-')),
      row('download (SwitchU DDS)', theme.packageBytes ? megabytes(theme.packageBytes) : megabytes(theme.bytes)),
      row('espaço console (SwitchU)', theme.installedBytes ? megabytes(theme.installedBytes) : '-'),
      row('licença', theme.license || 'não declarada'),
    );
    info.append(spec);

    if (!theme.license) {
      info.append(element('p', 'notice',
        'Sem licença declarada. O tema não deve ser promovido sem licença e origem da arte.'));
    }

    const manifestPath = safePath(theme.manifest);
    if (manifestPath) {
      const endpoint = element('div', 'endpoint');
      endpoint.append(
        element('b', '', 'manifesto que o console baixa'),
        element('span', '', `${location.origin}${manifestPath}`),
      );
      info.append(endpoint);
    }

    detail.append(visual, info);
    output.append(detail);
    startLoopMeters();
  }

  /* -------------------------------------------------------------- routing */

  function paint(render) {
    // View Transitions where supported; a plain repaint everywhere else.
    if (typeof document.startViewTransition === 'function'
        && !window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
      document.startViewTransition(render);
    } else {
      render();
    }
  }

  async function route() {
    let id = '';
    try { id = decodeURIComponent(location.hash.slice(1)); } catch { /* ignored */ }
    window.scrollTo(0, 0);

    const theme = themes.find(item => String(item.id) === id);
    if (!theme) { paint(renderGrid); return; }

    clear('state');
    output.textContent = 'Lendo manifesto…';
    const manifestPath = safePath(theme.manifest);
    let manifest = {};
    try {
      const response = await fetch(manifestPath, { cache: 'no-cache', credentials: 'same-origin' });
      if (response.ok) manifest = await response.json();
    } catch { /* the dossier still renders from the catalogue entry alone */ }
    paint(() => renderDetail(theme, manifest));
  }

  /* --------------------------------------------------------------- wiring */

  motionToggle.addEventListener('click', () => {
    motionEnabled = !motionEnabled;
    try { localStorage.setItem('switchu-theme-motion', motionEnabled ? 'on' : 'off'); } catch { /* optional */ }
    renderMotionToggle();
    route();
  });

  let searchTimer = 0;
  searchInput.addEventListener('input', () => {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(() => {
      query = searchInput.value;
      if (!location.hash) renderGrid();
    }, 120);
  });

  chips.forEach(chip => chip.addEventListener('click', () => {
    activeFilter = chip.dataset.filter || 'todos';
    chips.forEach(other => other.setAttribute('aria-pressed', String(other === chip)));
    if (!location.hash) renderGrid();
  }));

  document.addEventListener('keydown', event => {
    if (event.key === 'Escape' && location.hash) location.hash = '';
  });
  window.addEventListener('hashchange', route);

  renderMotionToggle();
  fetch('review.json', { cache: 'no-cache', credentials: 'same-origin' })
    .then(response => (response.ok ? response.json() : Promise.reject(new Error(`HTTP ${response.status}`))))
    .then(data => {
      themes = Array.isArray(data?.themes) ? data.themes : [];
      renderTotals();
      route();
    })
    .catch(() => {
      clear('state bad');
      output.textContent = 'Não consegui ler o catálogo.';
    });
})();
