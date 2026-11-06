"use client";

import { useEffect, useState } from "react";
import { fmt } from "../../lib/format";
import { ndviColor, SEVERITY } from "../../lib/scale";
import type { Analysis, EntrySummary, Severity } from "../../lib/types";
import { NdviHistogram, NdviSeries } from "./Charts";
import { Icon, type IconName } from "./Icon";
import { ImageCompare } from "./ImageCompare";
import { AttitudeIndicator, Compass } from "./Instruments";
import type { MissionView } from "./Sidebar";

function Stat({ label, value, unit, icon, accent }: { label: string; value: React.ReactNode; unit?: string; icon?: IconName; accent?: string }) {
  return (
    <div className="stat">
      <span className="stat-label">{icon && <Icon name={icon} size={13} />}{label}</span>
      <span className="stat-value">
        {accent && <i className="stat-swatch" style={{ background: accent }} />}
        {value}
        {unit && <small>{unit}</small>}
      </span>
    </div>
  );
}

function SeverityBadge({ severity }: { severity: Severity }) {
  return (
    <span className={`severity sev-${severity}`} style={{ ["--sev" as string]: SEVERITY[severity].color }}>
      <Icon name={severity === "alta" ? "alert" : "target"} size={12} />
      {SEVERITY[severity].label}
    </span>
  );
}

// Campos extras do metadata (fora do contrato) em linhas "a.b.c → valor".
function flatten(value: Record<string, unknown>, prefix = ""): [string, unknown][] {
  return Object.entries(value).flatMap(([k, v]) =>
    v && typeof v === "object" && !Array.isArray(v) ? flatten(v as Record<string, unknown>, `${prefix}${k}.`) : [[`${prefix}${k}`, Array.isArray(v) ? JSON.stringify(v) : v] as [string, unknown]],
  );
}

// ===== Detalhe de uma foto =====

type Tab = "analise" | "focos" | "telemetria" | "arquivos";

export function CaptureDetail({
  entry,
  position,
  total,
  selectedHotspot,
  onSelectHotspot,
  onStep,
  onClose,
}: {
  entry: EntrySummary;
  position: number;
  total: number;
  selectedHotspot: number | null;
  onSelectHotspot: (id: number) => void;
  onStep: (delta: number) => void;
  onClose: () => void;
}) {
  const [analysis, setAnalysis] = useState<Analysis | null>(null);
  const [metadata, setMetadata] = useState<Record<string, unknown> | null>(null);
  const [tab, setTab] = useState<Tab>("analise");

  useEffect(() => {
    let cancelled = false;
    setAnalysis(null);
    if (entry.status === "processed") {
      fetch(`/api/entries/${entry.id}/analysis.json?v=${entry.version}`)
        .then((r) => (r.ok ? r.json() : null))
        .then((a) => !cancelled && setAnalysis(a))
        .catch(() => undefined);
    }
    return () => { cancelled = true; };
  }, [entry.id, entry.version, entry.status]);

  useEffect(() => {
    let cancelled = false;
    fetch(`/api/entries/${entry.id}/metadata.json`)
      .then((r) => (r.ok ? r.json() : null))
      .then((m) => !cancelled && setMetadata(m))
      .catch(() => undefined);
    return () => { cancelled = true; };
  }, [entry.id]);

  useEffect(() => {
    if (selectedHotspot !== null) setTab("focos");
  }, [selectedHotspot]);

  useEffect(() => {
    const key = (event: KeyboardEvent) => {
      if ((event.target as HTMLElement).closest("input, textarea, [role=slider]")) return;
      if (event.key === "ArrowLeft") onStep(-1);
      if (event.key === "ArrowRight") onStep(1);
      if (event.key === "Escape") onClose();
    };
    window.addEventListener("keydown", key);
    return () => window.removeEventListener("keydown", key);
  }, [onStep, onClose]);

  const ndvi = analysis?.ndvi;
  const hotspots = analysis?.hotspots ?? [];
  const pos = entry.position;
  const orientation = entry.orientation;
  const known = new Set(["schema", "id", "mission_id", "sequence", "timestamp", "position", "orientation"]);
  const extra = metadata ? flatten(Object.fromEntries(Object.entries(metadata).filter(([k]) => !known.has(k)))) : [];

  return (
    <div className="detail">
      <header className="detail-header">
        <div>
          <span className="eyebrow">{entry.sequence != null ? `Foto ${entry.sequence} de ${total}` : `Captura ${position + 1} de ${total}`}</span>
          <h2>{fmt.time(entry.timestamp)}<small>{fmt.date(entry.timestamp)}</small></h2>
        </div>
        <div className="detail-nav">
          <button className="icon-btn" onClick={() => onStep(-1)} disabled={position <= 0} aria-label="Foto anterior"><Icon name="left" size={16} /></button>
          <button className="icon-btn" onClick={() => onStep(1)} disabled={position >= total - 1} aria-label="Próxima foto"><Icon name="right" size={16} /></button>
          <button className="icon-btn" onClick={onClose} aria-label="Fechar"><Icon name="x" size={16} /></button>
        </div>
      </header>

      <ImageCompare entry={entry} analysis={analysis} selectedHotspot={selectedHotspot} onSelectHotspot={onSelectHotspot} />

      {entry.position?.source === "imu" && (
        <div className="notice"><Icon name="alert" size={14} />Posição estimada só pela IMU (sem fix GPS){entry.position.sigma_m != null ? `, ±${fmt.number(entry.position.sigma_m, 0)} m` : ""}. Pegada e focos podem estar deslocados no mapa.</div>
      )}
      {analysis?.warnings.map((w) => <div key={w} className="notice"><Icon name="alert" size={14} />{w}</div>)}

      <div className="stats-grid">
        <Stat label="NDVI médio" value={fmt.signed(entry.ndvi?.mean ?? null)} accent={entry.ndvi ? ndviColor(entry.ndvi.mean) : undefined} icon="leaf" />
        <Stat label="Vegetação" value={fmt.percent(entry.ndvi?.vegetation_fraction ?? null)} icon="layers" />
        <Stat label="Focos" value={entry.status === "processed" ? entry.hotspots.length : "—"} icon="target" />
        <Stat label="Resolução" value={fmt.number(entry.gsdCm, 1)} unit="cm/px" icon="scan" />
      </div>

      <div className="tabs" role="tablist">
        {([
          ["analise", "Análise"],
          ["focos", `Focos${hotspots.length ? ` · ${hotspots.length}` : ""}`],
          ["telemetria", "Telemetria"],
          ["arquivos", "Arquivos"],
        ] as [Tab, string][]).map(([id, label]) => (
          <button key={id} role="tab" aria-selected={tab === id} className={tab === id ? "active" : ""} onClick={() => setTab(id)}>{label}</button>
        ))}
      </div>

      <div className="tab-panel">
        {tab === "analise" && (
          ndvi ? (
            <>
              <h3 className="panel-title">Distribuição do NDVI</h3>
              <NdviHistogram edges={ndvi.histogram.edges} counts={ndvi.histogram.counts} median={ndvi.median} threshold={analysis?.detection.vegetation_threshold ?? null} />
              <dl className="kv">
                <div><dt>Percentis 10 – 90</dt><dd className="mono">{fmt.signed(ndvi.p10)} … {fmt.signed(ndvi.p90)}</dd></div>
                <div><dt>Desvio padrão</dt><dd className="mono">{fmt.number(ndvi.std, 3)}</dd></div>
                <div><dt>Pixels válidos</dt><dd className="mono">{fmt.percent(ndvi.valid_fraction, 1)}</dd></div>
                <div><dt>Área no solo</dt><dd className="mono">{fmt.area(analysis?.geo.area_m2)}</dd></div>
              </dl>
            </>
          ) : (
            <p className="muted-note">Análise disponível após o processamento.</p>
          )
        )}

        {tab === "focos" && (
          hotspots.length ? (
            <ol className="hotspot-list">
              {hotspots.map((h) => (
                <li key={h.id}>
                  <button className={selectedHotspot === h.id ? "selected" : ""} onClick={() => onSelectHotspot(h.id)}>
                    <span className="hotspot-id" style={{ ["--sev" as string]: SEVERITY[h.severity].color }}>{h.id}</span>
                    <span className="hotspot-main">
                      <span className="hotspot-ndvi">
                        <small>foco</small><i style={{ background: ndviColor(h.ndvi_mean) }} />{fmt.signed(h.ndvi_mean)}
                        <small>entorno</small><i style={{ background: ndviColor(h.surrounding_ndvi_mean) }} />{fmt.signed(h.surrounding_ndvi_mean)}
                      </span>
                      <small>Queda de {fmt.number(h.ndvi_drop, 2)} · {h.area_m2 != null ? `${fmt.number(h.area_m2, 1)} m²` : `${h.area_px} px`}</small>
                    </span>
                    <SeverityBadge severity={h.severity} />
                  </button>
                </li>
              ))}
            </ol>
          ) : (
            <div className="empty-inline"><Icon name="check" size={18} /><span>{entry.status === "processed" ? "Nenhum foco de NDVI baixo nesta foto." : "Análise disponível após o processamento."}</span></div>
          )
        )}

        {tab === "telemetria" && (
          <>
            <div className="instruments">
              <AttitudeIndicator roll={orientation?.roll_deg ?? null} pitch={orientation?.pitch_deg ?? null} />
              <Compass heading={orientation?.yaw_deg ?? null} />
            </div>
            <dl className="kv">
              <div><dt>Norte</dt><dd className="mono">{fmt.number(pos?.north_m, 1)} m</dd></div>
              <div><dt>Leste</dt><dd className="mono">{fmt.number(pos?.east_m, 1)} m</dd></div>
              <div><dt>Altura</dt><dd className="mono">{fmt.number(pos?.height_m, 1)} m</dd></div>
              <div>
                <dt>Fonte da posição</dt>
                <dd className={`mono ${pos?.source === "imu" ? "warn-text" : ""}`}>
                  {pos?.source === "imu" ? "só IMU (sem GPS)" : pos?.source === "gps+imu" ? "GPS + IMU" : "—"}
                  {pos?.sigma_m != null ? ` · ±${fmt.number(pos.sigma_m, 1)} m` : ""}
                </dd>
              </div>
              <div><dt>Distância da estação</dt><dd className="mono">{pos?.north_m != null && pos.east_m != null ? `${fmt.number(Math.hypot(pos.north_m, pos.east_m), 1)} m` : "—"}</dd></div>
              {extra.map(([k, v]) => (
                <div key={k}><dt>{k}</dt><dd className="mono small">{typeof v === "number" ? fmt.number(v, 3) : v === null ? "—" : String(v)}</dd></div>
              ))}
            </dl>
          </>
        )}

        {tab === "arquivos" && (
          <div className="file-grid">
            {[
              ["rgb.jpg", "RGB original", "image"],
              ["nir.jpg", "NIR original", "image"],
              ["ndvi.npy", "NDVI (uint8)", "layers"],
              ["metadata.json", "Metadados", "pin"],
              ["analysis.json", "Análise", "sparkle"],
            ].map(([file, label, icon]) => (
              <a key={file} href={`/api/entries/${entry.id}/${file}?download`} className="file-link">
                <Icon name={icon as IconName} size={16} />
                <span><strong>{label}</strong><small>{file}</small></span>
                <Icon name="download" size={15} />
              </a>
            ))}
            <p className="muted-note mono small">{entry.id}</p>
          </div>
        )}
      </div>
    </div>
  );
}

// ===== Resumo da missão =====

export function MissionOverview({
  mission,
  selectedEntry,
  onSelectEntry,
  onSelectHotspot,
  now,
}: {
  mission: MissionView;
  selectedEntry: string | null;
  onSelectEntry: (id: string) => void;
  onSelectHotspot: (entryId: string, id: number) => void;
  now: number;
}) {
  const processed = mission.entries.filter((e) => e.ndvi);
  const bySeverity = { alta: 0, media: 0, baixa: 0 } as Record<Severity, number>;
  mission.clusters.forEach((c) => bySeverity[c.severity]++);
  const top = mission.clusters.slice(0, 8); // já ordenados por severidade e queda
  const end = mission.finishedAt ? Date.parse(mission.finishedAt) : mission.live ? now : null;

  return (
    <div className="detail overview">
      <header className="detail-header">
        <div>
          <span className="eyebrow">{mission.live ? "Missão em andamento" : mission.id === "loose" ? "Sem missão" : "Resumo da missão"}</span>
          <h2>{mission.label}<small>{mission.startedAt ? `${fmt.date(mission.startedAt)} · ${fmt.time(mission.startedAt, false)}` : ""}</small></h2>
        </div>
        {mission.forced && <span className="tag tag-warn">finalizada à força</span>}
      </header>

      <div className="hero-stats">
        <div className="hero">
          <span>NDVI médio</span>
          <strong className={mission.meanNdvi === null ? "no-swatch" : ""} style={{ ["--swatch" as string]: mission.meanNdvi !== null ? ndviColor(mission.meanNdvi) : "transparent" }}>
            {fmt.signed(mission.meanNdvi)}
          </strong>
        </div>
        <div className="hero">
          <span>Focos únicos</span>
          <strong>{mission.hotspots}</strong>
          <div className="severity-bar" aria-label="Focos por severidade">
            {(["alta", "media", "baixa"] as Severity[]).map((s) => bySeverity[s] > 0 && (
              <i key={s} style={{ flex: bySeverity[s], background: SEVERITY[s].color }} title={`${SEVERITY[s].label}: ${bySeverity[s]}`} />
            ))}
          </div>
        </div>
      </div>

      <div className="stats-grid">
        <Stat
          label="Fotos"
          value={mission.declaredCaptures != null && mission.declaredCaptures !== mission.entries.length ? `${mission.entries.length}/${mission.declaredCaptures}` : mission.entries.length}
          icon="camera"
        />
        <Stat label="Área coberta" value={fmt.area(mission.areaM2 || null)} icon="scan" />
        <Stat label="Duração" value={mission.startedAt && end ? fmt.duration(end - Date.parse(mission.startedAt)) : "—"} icon="clock" />
        <Stat label="Processando" value={mission.processing} icon="sparkle" />
      </div>

      <div className="severity-legend">
        {(["alta", "media", "baixa"] as Severity[]).map((s) => (
          <span key={s}><SeverityBadge severity={s} /><strong className="mono">{bySeverity[s]}</strong></span>
        ))}
      </div>

      {mission.id !== "loose" && (
        <section className="track-card">
          <div className="track-card-head">
            <span className="track-icon"><Icon name="route" size={16} /></span>
            <div>
              <strong>Histórico de posições</strong>
              <span>{mission.track ? `${mission.track.samples.toLocaleString("pt-BR")} posições registradas` : mission.live ? "Enviado pelo módulo ao fim da missão" : "Não recebido do módulo"}</span>
            </div>
            <span className={`track-status ${mission.track ? "ok" : ""}`}>
              <Icon name={mission.track ? "check" : "clock"} size={12} />
              {mission.track ? "Registrado" : "Pendente"}
            </span>
          </div>
          {mission.track && (
            <dl className="track-stats">
              <div><dt>Percorrido</dt><dd className="mono">{fmt.distance(mission.track.distance_m)}</dd></div>
              <div><dt>Alcance máx.</dt><dd className="mono">{fmt.distance(mission.track.max_range_m)}</dd></div>
              <div><dt>Velocidade média</dt><dd className="mono">{mission.track.duration_s > 0 ? `${fmt.number(mission.track.distance_m / mission.track.duration_s, 1)} m/s` : "—"}</dd></div>
            </dl>
          )}
        </section>
      )}

      <section className="panel-section">
        <h3 className="panel-title">NDVI ao longo da missão</h3>
        <NdviSeries
          points={processed.map((e, i) => ({ id: e.id, sequence: e.sequence ?? i + 1, value: e.ndvi!.mean, hotspots: e.hotspots.length }))}
          selected={selectedEntry}
          onSelect={onSelectEntry}
        />
      </section>

      <section className="panel-section">
        <h3 className="panel-title">Focos prioritários</h3>
        {top.length ? (
          <ol className="hotspot-list">
            {top.map((c, i) => (
              <li key={c.key}>
                <button onClick={() => onSelectHotspot(c.best.entryId, c.best.id)}>
                  <span className="hotspot-id" style={{ ["--sev" as string]: SEVERITY[c.severity].color }}>{i + 1}</span>
                  <span className="hotspot-main">
                    <span>Queda de NDVI de {fmt.number(c.ndvi_drop, 2)}</span>
                    <small>
                      {c.area_m2 != null ? `${fmt.number(c.area_m2, 1)} m²` : "área desconhecida"}
                      {c.north_m != null && c.east_m != null ? ` · ${fmt.number(Math.hypot(c.north_m, c.east_m), 0)} m da estação` : ""}
                      {` · ${c.refs.length > 1 ? `fotos ${c.refs.map((r) => r.sequence ?? "—").join(", ")}` : `foto ${c.best.sequence ?? "—"}`}`}
                      {c.uncertain && <span className="tag tag-warn inline">posição incerta</span>}
                    </small>
                  </span>
                  <SeverityBadge severity={c.severity} />
                </button>
              </li>
            ))}
          </ol>
        ) : (
          <div className="empty-inline"><Icon name="leaf" size={18} /><span>{mission.entries.length ? "Nenhum foco de NDVI baixo. Vegetação homogênea." : "Os focos aparecem quando as fotos forem processadas."}</span></div>
        )}
      </section>
    </div>
  );
}
