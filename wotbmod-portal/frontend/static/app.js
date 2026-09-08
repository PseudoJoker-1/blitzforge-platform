/* BlitzForge portal. Everything works without this file; with it: a toast for
   outcomes, live search on the catalogue, copy buttons, the install hand-off
   state, one-click ratings and uploads without a full reload. */
(function () {
  "use strict";
  const $ = (sel, root) => (root || document).querySelector(sel);
  const $$ = (sel, root) => Array.from((root || document).querySelectorAll(sel));
  const csrf = document.body.dataset.csrf || "";

  let toastTimer = null;
  function toast(text, kind) {
    let el = $(".toast");
    if (!el) { el = document.createElement("div"); el.className = "toast"; el.setAttribute("role", "status"); document.body.appendChild(el); }
    el.textContent = text;
    el.className = "toast" + (kind === "error" ? " error" : "");
    requestAnimationFrame(() => el.classList.add("show"));
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.classList.remove("show"), 4000);
  }
  if (document.body.dataset.flash) toast(document.body.dataset.flash, document.body.dataset.flashKind);

  $$("[data-copy]").forEach((button) => button.addEventListener("click", async () => {
    try { await navigator.clipboard.writeText(button.dataset.copy); toast("Скопировано"); }
    catch (_) { toast("Не удалось скопировать, выделите текст вручную", "error"); }
  }));

  async function api(method, url, body) {
    const headers = { "Accept": "application/json", "X-CSRF-Token": csrf };
    let payload = body;
    if (body && !(body instanceof FormData)) { headers["Content-Type"] = "application/json"; payload = JSON.stringify(body); }
    const response = await fetch(url, { method, headers, body: payload, credentials: "same-origin" });
    let data = {};
    try { data = await response.json(); } catch (_) { data = { ok: response.ok }; }
    if (!response.ok) throw new Error(data.error || ("HTTP " + response.status));
    return data;
  }

  // Live search: the list re-renders from /api/v1/mods as you type or filter.
  const search = $("#q");
  const list = $("#grid");
  if (search && list) {
    const state = { q: search.value || "", type: "", tier: "", verified: "", sort: ($("#sort") || {}).value || "updated" };
    $$(".filters .toggle[data-filter]").forEach((toggle) => {
      if (toggle.getAttribute("aria-pressed") === "true") state[toggle.dataset.filter] = toggle.dataset.value;
      toggle.addEventListener("click", () => {
        const on = toggle.getAttribute("aria-pressed") === "true";
        $$('.filters .toggle[data-filter="' + toggle.dataset.filter + '"]').forEach((other) => other.setAttribute("aria-pressed", "false"));
        toggle.setAttribute("aria-pressed", on ? "false" : "true");
        state[toggle.dataset.filter] = on ? "" : toggle.dataset.value;
        load();
      });
    });
    const sort = $("#sort");
    if (sort) sort.addEventListener("change", () => { state.sort = sort.value; load(); });
    let timer = null;
    search.addEventListener("input", () => { state.q = search.value; clearTimeout(timer); timer = setTimeout(load, 220); });
    const form = $("#searchform");
    if (form) form.addEventListener("submit", (event) => { event.preventDefault(); clearTimeout(timer); load(); });

    const TIER_LABELS = { SAFE: "безопасные права", GAMEPLAY_TWEAK: "HUD и камера", REVIEWED: "нужна проверка", UNSAFE: "полный доступ" };
    function esc(text) { return String(text).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c])); }
    function initials(name) { const parts = name.replace(/^[a-z0-9_-]+\./i, "").split(/[\s._-]+/).filter(Boolean); return (parts.slice(0, 2).map((p) => p[0]).join("") || name.slice(0, 2)).toUpperCase(); }
    function plural(n, one, few, many) { const m10 = n % 10, m100 = n % 100; if (m10 === 1 && m100 !== 11) return one; if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return few; return many; }
    function rating(mod) { return mod.votes ? mod.rating + " из 5, " + mod.votes + " " + plural(mod.votes, "оценка", "оценки", "оценок") : "нет оценок"; }
    function row(mod) {
      const href = "/mods/" + encodeURIComponent(mod.id);
      return '<li class="row">' +
        '<a class="mark" href="' + href + '" aria-hidden="true" tabindex="-1">' + esc(initials(mod.name)) + "</a>" +
        '<div><div class="name"><a href="' + href + '">' + esc(mod.name) + "</a>" + (mod.verified ? '<span class="verified">проверен</span>' : "") + "</div>" +
        '<div class="sub">' + esc(mod.developer) + (mod.summary ? ". " + esc(mod.summary) : "") + "</div></div>" +
        '<div class="meta"><b>' + esc(mod.version) + "</b>" + rating(mod) + ", " + mod.downloads + " " + plural(mod.downloads, "загрузка", "загрузки", "загрузок") +
        '<br><span class="tier-label" title="' + esc(mod.tier) + '">' + esc(TIER_LABELS[mod.tier] || mod.tier) + "</span></div></li>";
    }
    async function load() {
      const params = new URLSearchParams();
      for (const key of ["q", "type", "tier", "verified", "sort"]) if (state[key]) params.set(key, state[key]);
      history.replaceState(null, "", "/" + (params.toString() ? "?" + params : ""));
      list.setAttribute("aria-busy", "true");
      try {
        const data = await api("GET", "/api/v1/mods?" + params);
        const count = $("#count");
        if (count) count.textContent = data.mods.length ? data.mods.length + " " + plural(data.mods.length, "мод", "мода", "модов") : "ничего не найдено";
        list.innerHTML = data.mods.length ? data.mods.map(row).join("") : '<li class="empty">По этому запросу модов нет. Попробуйте другое слово или снимите фильтры.</li>';
      } catch (error) { toast("Каталог недоступен: " + error.message, "error"); }
      list.removeAttribute("aria-busy");
    }
  }

  // Install hand-off: the browser asks the launcher; the page says what happened.
  $$("a.install[data-scheme]").forEach((button) => button.addEventListener("click", () => {
    const label = button.querySelector("span");
    const original = label ? label.textContent : "";
    button.setAttribute("aria-busy", "true");
    if (label) label.textContent = "Открываем установщик";
    const spinner = document.createElement("i"); spinner.className = "spinner"; button.prepend(spinner);
    let handedOff = false;
    const onBlur = () => { handedOff = true; };
    window.addEventListener("blur", onBlur, { once: true });
    setTimeout(() => {
      window.removeEventListener("blur", onBlur);
      spinner.remove();
      button.removeAttribute("aria-busy");
      if (label) label.textContent = original;
      if (handedOff) { toast("Установщик открыт: подтвердите установку в его окне"); return; }
      const manual = $("#manual");
      if (manual) { manual.open = true; manual.scrollIntoView({ behavior: "smooth", block: "center" }); }
      toast("Установщик не ответил. Ниже описан ручной путь.", "error");
    }, 2600);
  }));

  // Forms that answer in place (upload, tokens).
  $$("form[data-async]").forEach((form) => form.addEventListener("submit", async (event) => {
    event.preventDefault();
    const button = form.querySelector("button[type=submit], button:not([type])");
    if (button) button.disabled = true;
    try {
      const isUpload = form.enctype === "multipart/form-data";
      const body = isUpload ? new FormData(form) : Object.fromEntries(new FormData(form).entries());
      const data = await api(form.method.toUpperCase() || "POST", form.action, body);
      toast(form.dataset.done || "Готово");
      if (form.dataset.reload !== "no") setTimeout(() => location.reload(), 700);
      if (data.token) { const box = $("#tokenbox"); if (box) { box.textContent = data.token; box.parentElement.hidden = false; } }
    } catch (error) { toast(error.message, "error"); }
    if (button) button.disabled = false;
  }));

  // Rating: choosing a number saves it.
  $$(".rating-pick input").forEach((input) => input.addEventListener("change", async () => {
    const form = input.closest("form");
    try { await api("POST", form.action, { stars: input.value }); toast("Оценка сохранена"); }
    catch (error) { toast(error.message, "error"); }
  }));
})();
