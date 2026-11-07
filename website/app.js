(() => {
  const legacy = /^#\/[a-z]{2}\//.test(location.hash);
  if (legacy) { location.replace('Documentation.html' + location.hash); return; }
  const root = document.querySelector('#page');
  let locale = new URL(location.href).searchParams.get('lang') === 'fr' ? 'fr' : 'en';
  let chapterObserver, launcher, zoomIndex = 0, zoomItems = [];
  const dialog = () => root.querySelector('.lightbox');
  function setup() {
    chapterObserver?.disconnect();
    chapterObserver = new IntersectionObserver(entries => {
      const entry = entries.filter(item => item.isIntersecting).sort((a,b) => a.boundingClientRect.top - b.boundingClientRect.top)[0];
      if (!entry) return;
      for (const link of root.querySelectorAll('[data-chapter]')) {
        if (link.dataset.chapter === entry.target.id) link.setAttribute('aria-current','location'); else link.removeAttribute('aria-current');
      }
    }, {rootMargin:'-20% 0px -55% 0px',threshold:0});
    for (const section of root.querySelectorAll('section[id]')) chapterObserver.observe(section);
    dialog().addEventListener('cancel', event => { event.preventDefault(); closeImage(); });
  }
  function setLocale(next, updateUrl = true) {
    if (!['en','fr'].includes(next)) return;
    const y = window.scrollY;
    locale = next;
    const template = document.querySelector(`#locale-${locale}`);
    root.replaceChildren(template.content.cloneNode(true));
    document.documentElement.lang = locale;
    document.title = template.dataset.title;
    document.querySelector('meta[name=description]').content = template.dataset.description;
    document.querySelector('meta[property="og:title"]').content = template.dataset.title;
    document.querySelector('meta[property="og:description"]').content = template.dataset.description;
    if (updateUrl) { const url = new URL(location.href); url.searchParams.set('lang',locale); history.replaceState(null,'',url); }
    setup();
    if (updateUrl) requestAnimationFrame(() => window.scrollTo({top:y,behavior:'instant'}));
  }
  function activateTab(index, focus = false) {
    const tabs = [...root.querySelectorAll('[role=tab]')];
    const selected = ((index % tabs.length) + tabs.length) % tabs.length;
    tabs.forEach((tab,i) => {
      tab.setAttribute('aria-selected',String(i === selected)); tab.tabIndex = i === selected ? 0 : -1;
      root.querySelector(`#${tab.getAttribute('aria-controls')}`).hidden = i !== selected;
    });
    if (focus) tabs[selected].focus();
  }
  function showImage(index) {
    zoomIndex = (index + zoomItems.length) % zoomItems.length;
    const item = zoomItems[zoomIndex];
    dialog().querySelector('[data-lightbox-image]').src = item.dataset.zoom;
    dialog().querySelector('[data-lightbox-image]').alt = item.dataset.caption;
    dialog().querySelector('[data-lightbox-caption]').textContent = item.dataset.caption;
    dialog().querySelector('[data-lightbox-count]').textContent = `${String(zoomIndex+1).padStart(2,'0')} / ${String(zoomItems.length).padStart(2,'0')}`;
  }
  function openImage(element) {
    launcher = element;
    zoomItems = [...root.querySelectorAll('[data-zoom]')].filter(item => !item.closest('[hidden]'));
    showImage(zoomItems.indexOf(element));
    dialog().showModal(); document.documentElement.classList.add('lightbox-open');
    dialog().querySelector('[data-lightbox-close]').focus();
  }
  function closeImage() {
    if (!dialog()?.open) return;
    dialog().close(); document.documentElement.classList.remove('lightbox-open');
    dialog().querySelector('[data-lightbox-image]').removeAttribute('src');
    launcher?.focus({preventScroll:true});
  }
  function closeMenu() {
    root.querySelector('#primary-nav').classList.remove('is-open'); root.querySelector('[data-menu]').setAttribute('aria-expanded','false');
  }
  root.addEventListener('click', event => {
    const language = event.target.closest('[data-language]');
    if (language) { event.preventDefault(); setLocale(language.dataset.language); return; }
    const menu = event.target.closest('[data-menu]');
    if (menu) { const open = menu.getAttribute('aria-expanded') !== 'true'; menu.setAttribute('aria-expanded',String(open)); root.querySelector('#primary-nav').classList.toggle('is-open',open); return; }
    const tab = event.target.closest('[role=tab]');
    if (tab) { activateTab([...root.querySelectorAll('[role=tab]')].indexOf(tab)); return; }
    const image = event.target.closest('[data-zoom]');
    if (image) { openImage(image); return; }
    if (event.target.closest('[data-lightbox-close]') || event.target === dialog()) closeImage();
    if (event.target.closest('[data-lightbox-prev]')) showImage(zoomIndex-1);
    if (event.target.closest('[data-lightbox-next]')) showImage(zoomIndex+1);
    if (event.target.closest('a[href^="#"]')) closeMenu();
    if (!event.target.closest('.site-header')) closeMenu();
  });
  root.addEventListener('input', event => {
    if (!event.target.matches('[data-compare]')) return;
    const figure = event.target.closest('.comparison');
    figure.querySelector('.comparison-images').style.setProperty('--split',`${event.target.value}%`);
    figure.querySelector('output').textContent = `${event.target.value}%`;
  });
  document.addEventListener('keydown', event => {
    if (dialog()?.open) {
      if (event.key === 'Escape') { event.preventDefault(); closeImage(); }
      if (event.key === 'ArrowLeft') { event.preventDefault(); showImage(zoomIndex-1); }
      if (event.key === 'ArrowRight') { event.preventDefault(); showImage(zoomIndex+1); }
      return;
    }
    if (event.key === 'Escape') closeMenu();
    const tab = event.target.closest?.('[role=tab]');
    if (tab && ['ArrowLeft','ArrowRight','Home','End'].includes(event.key)) {
      event.preventDefault(); const index = [...root.querySelectorAll('[role=tab]')].indexOf(tab);
      activateTab(event.key==='Home'?0:event.key==='End'?5:index+(event.key==='ArrowLeft'?-1:1), true);
    }
  });
  if (locale === 'fr') setLocale(locale,false); else setup();
})();
