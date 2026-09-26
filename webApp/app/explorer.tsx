"use client";

import { useState, useTransition } from "react";
import Image from "next/image";
import { useRouter } from "next/navigation";
import logo from "../assets/logo_white.png";
import type { Entry, Metadata } from "../lib/entries";

const paths = {
  layers: "m12 3 9 5-9 5-9-5 9-5Zm-9 9 9 5 9-5M3 16l9 5 9-5",
  refresh: "M20 7v5h-5M4 17v-5h5M6 7a7 7 0 0 1 12-1l2 3M4 15l2 3a7 7 0 0 0 12-1",
  arrow: "M7 17 17 7M7 7h10v10",
  image: "M4 4h16v16H4zM4 16l5-5 4 4 3-3 4 4M8 8h.01",
  pin: "M19 10c0 5-7 11-7 11S5 15 5 10a7 7 0 1 1 14 0ZM12 7a3 3 0 1 0 0 6 3 3 0 0 0 0-6Z",
  compass: "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18Zm4 5-2 6-6 2 2-6 6-2Z",
  pulse: "M2 12h5l3-8 4 16 3-8h5",
  clock: "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18Zm0 4v5l3 2",
};
type IconName = keyof typeof paths;

function Icon({ name, size = 18 }: { name: IconName; size?: number }) {
  return <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d={paths[name]} /></svg>;
}

function dateParts(value: unknown) {
  const date = typeof value === "string" ? new Date(value) : null;
  if (!date || !Number.isFinite(date.getTime())) return { day: "Data indisponível", time: "—" };
  return {
    day: date.toLocaleDateString("pt-BR", { day: "2-digit", month: "short", year: "numeric", timeZone: "UTC" }),
    time: date.toLocaleTimeString("pt-BR", { hour12: false, timeZone: "UTC" }),
  };
}

const labels: Record<string, string> = {
  image: "Imagem", width: "Largura", height: "Altura", position: "Posição",
  latitude: "Latitude", longitude: "Longitude", altitude_m: "Altitude",
  attitude: "Atitude", quaternion: "Quaternion", roll_deg: "Roll", pitch_deg: "Pitch",
  yaw_deg: "Yaw", heading_deg: "Heading", stability: "Estabilidade",
  gyro_rad_s: "Giroscópio · rad/s", accel_m_s2: "Aceleração · m/s²", angular_speed_rad_s: "Velocidade angular",
};
const units: Record<string, string> = { width: "px", height: "px", latitude: "°", longitude: "°", altitude_m: "m", roll_deg: "°", pitch_deg: "°", yaw_deg: "°", heading_deg: "°", angular_speed_rad_s: "rad/s" };

function MetadataRows({ data }: { data: Metadata }) {
  return <dl className="metadata-rows">{Object.entries(data).map(([key, value]) => {
    if (value !== null && typeof value === "object" && !Array.isArray(value)) {
      return <div className="metadata-nested" key={key}><dt>{labels[key] || key}</dt><dd><MetadataRows data={value} /></dd></div>;
    }
    const unavailable = value === null || value === undefined;
    const formatted = unavailable ? "—" : typeof value === "number" ? value.toLocaleString("pt-BR", { maximumFractionDigits: 6 }) : typeof value === "object" ? JSON.stringify(value) : String(value);
    return <div className="metadata-row" key={key}><dt>{labels[key] || key}</dt><dd className={unavailable ? "missing" : ""} title={unavailable ? "Indisponível" : undefined}>{formatted}{!unavailable && units[key] && <small> {units[key]}</small>}</dd></div>;
  })}</dl>;
}

function Visual({ src, ndvi }: { src: string; ndvi?: boolean }) {
  const [status, setStatus] = useState<"loading" | "ready" | "error">("loading");
  const [dimensions, setDimensions] = useState("");
  return <article className="visual-card">
    <header className="visual-header"><div className={`visual-icon ${ndvi ? "green" : ""}`}><Icon name={ndvi ? "layers" : "image"} /></div><div><h2>{ndvi ? "Índice de vegetação" : "Imagem RGB"}</h2><p>{ndvi ? "NDVI · falso-colorido" : "Espectro visível"}</p></div><a href={src} target="_blank" rel="noreferrer" className="icon-button" title="Abrir imagem em tamanho original" aria-label="Abrir imagem em tamanho original"><Icon name="arrow" /></a></header>
    <div className="image-stage">
      {status === "loading" && <div className="image-message" role="status"><span className="spinner" />{ndvi ? "Renderizando NDVI…" : "Carregando imagem…"}</div>}
      {status === "error" && <div className="image-message"><Icon name="image" size={28} /><strong>Imagem indisponível</strong><span>Arquivo ausente ou formato inválido.</span></div>}
      <img src={src} alt={ndvi ? "NDVI da operação em escala de vermelho a verde" : "Captura RGB da operação"} className={status === "ready" ? "ready" : ""} onLoad={(event) => { setStatus("ready"); setDimensions(`${event.currentTarget.naturalWidth} × ${event.currentTarget.naturalHeight}`); }} onError={() => setStatus("error")} />
    </div>
    {ndvi ? <div className="legend"><div className="legend-title"><span>ÍNDICE NDVI</span><span>−1 a +1</span></div><div className="gradient" /><div className="legend-ticks"><span>−1</span><span>0</span><span>+1</span></div></div> : <div className="image-caption"><span className="file-tag">JPG</span><span>rgb.jpg</span><span className="dimensions">{dimensions || "Dimensões automáticas"}</span></div>}
    {ndvi && <div className="ndvi-caption">uint8 → NDVI <span>{dimensions}</span></div>}
  </article>;
}

export default function Explorer({ entries, error }: { entries: Entry[]; error?: string }) {
  const [selected, setSelected] = useState<string | null>(null);
  const [revision, setRevision] = useState(0);
  const [pending, startTransition] = useTransition();
  const router = useRouter();
  const entry = entries.find((e) => e.id === selected) || entries[0];
  const date = dateParts(entry?.metadata?.timestamp);
  const base = entry ? `/api/entries/${entry.id}` : "";
  const sections: [string, IconName][] = [["image", "image"], ["position", "pin"], ["attitude", "compass"], ["stability", "pulse"]];
  const extras = entry?.metadata ? Object.fromEntries(Object.entries(entry.metadata).filter(([key]) => !["id", "timestamp", ...sections.map(([name]) => name)].includes(key))) : {};

  function refresh() {
    startTransition(() => router.refresh());
    setRevision((value) => value + 1);
  }

  return <div className="app-shell">
    <aside className="navigation" aria-label="Navegação de operações">
      <a className="brand" href="/" aria-label="AtriumOne, início"><Image src={logo} alt="" width={34} height={40} priority /><span>AtriumOne<span className="brand-subtitle">Atrium</span></span></a>
      <div className="nav-heading"><span><Icon name="layers" size={16} /> Operações</span><span className="count">{entries.length.toString().padStart(2, "0")}</span></div>
      <div className="sort-label"><span>MAIS RECENTES PRIMEIRO</span><span>↓</span></div>
      <nav className="entry-list">{entries.map((item) => {
        const timestamp = dateParts(item.metadata?.timestamp);
        return <button key={item.id} className={`entry-button ${entry?.id === item.id ? "selected" : ""}`} onClick={() => setSelected(item.id)} aria-current={entry?.id === item.id ? "true" : undefined}>
          <span className="entry-marker" /><span className="entry-info"><span className="entry-date">{timestamp.day}</span><span className="entry-time">{timestamp.time} <small>UTC</small></span><span className="entry-id">{item.id.slice(0, 8)}{item.error ? " · metadata inválida" : ""}</span></span><span className="entry-chevron">›</span>
        </button>;
      })}{!entries.length && <p className="nav-empty">Nenhuma operação encontrada.</p>}</nav>
      <div className="nav-footer"><span className="folder-symbol">⌑</span><div><strong>Fonte local</strong><span>entries/</span></div><span className="local-dot" /></div>
    </aside>

    <div className="workspace">
      <header className="topbar"><div className="breadcrumb">Sensoriamento remoto <span>/</span><strong>Operações</strong></div><button className="refresh-button" onClick={refresh} disabled={pending}><span className={pending ? "rotating" : ""}><Icon name="refresh" size={15} /></span>{pending ? "Atualizando…" : "Atualizar"}</button></header>
      <div className="workspace-body">
        <main className="main-panel">
          {entry ? <>
            <div className="page-heading"><div><div className="eyebrow"><span /> OPERAÇÃO</div><h1>{date.day}</h1><p><Icon name="clock" size={14} />{date.time} UTC <span className="separator">/</span><span className="operation-id" title={entry.id}>{entry.id.slice(0, 8)}</span></p></div><span className="read-only">Somente leitura</span></div>
            {entry.error && <div className="notice" role="alert">{entry.error} As imagens continuam disponíveis para consulta.</div>}
            <div className="visual-grid"><Visual key={`${entry.id}-rgb-${revision}`} src={`${base}/rgb.jpg?v=${revision}`} /><Visual key={`${entry.id}-ndvi-${revision}`} src={`${base}/ndvi.png?v=${revision}`} ndvi /></div>
            <div className="files-section"><div className="section-label">ARQUIVOS DA OPERAÇÃO <span>03</span></div><div className="file-links">{["rgb.jpg", "ndvi.npy", "metadata.json"].map((file) => <a key={file} href={`${base}/${file}`} download={file}><span className={`file-extension ${file.endsWith("npy") ? "npy" : ""}`}>{file.split(".")[1]}</span><span>{file}</span><span className="download-arrow">↓</span></a>)}</div></div>
          </> : <div className="empty-state"><div className="empty-icon"><Icon name="layers" size={32} /></div><h1>{error ? "Pasta indisponível" : "Nenhuma operação encontrada"}</h1><p>{error || "Adicione os arquivos em entries/UUID/ e clique em Atualizar."}</p><code>entries/UUID/<br />├── rgb.jpg<br />├── ndvi.npy<br />└── metadata.json</code><button className="refresh-button" onClick={refresh} disabled={pending}><Icon name="refresh" size={15} /> Atualizar operações</button></div>}
        </main>

        {entry && <aside className="inspector" aria-label="Metadados da operação"><header className="inspector-header"><div><span className="eyebrow">DETALHES</span><h2>Metadados</h2></div><span className="json-badge">JSON</span></header><div className="inspector-scroll">
          <section className="identity-section"><span className="section-label">IDENTIFICADOR</span><code>{typeof entry.metadata?.id === "string" ? entry.metadata.id : entry.id}</code><span className="section-label">TIMESTAMP · UTC</span><p className="timestamp">{typeof entry.metadata?.timestamp === "string" ? entry.metadata.timestamp : "—"}</p></section>
          {sections.map(([key, icon]) => {
            const value = entry.metadata?.[key];
            return <section className="metadata-section" key={key}><h3><Icon name={icon} size={16} />{labels[key]}</h3>{value && typeof value === "object" && !Array.isArray(value) ? <MetadataRows data={value} /> : <p className="unavailable">Indisponível</p>}</section>;
          })}
          {Object.keys(extras).length > 0 && <section className="metadata-section"><h3>Outros dados</h3><MetadataRows data={extras} /></section>}
          <p className="metadata-note"><span>—</span> Campos sem leitura aparecem como indisponíveis.</p>
        </div><a className="raw-json" href={`${base}/metadata.json`} target="_blank" rel="noreferrer"><span>{"{ }"} <span>Ver JSON original</span></span><Icon name="arrow" size={15} /></a></aside>}
      </div>
    </div>
  </div>;
}
