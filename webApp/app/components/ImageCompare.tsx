"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import { fmt } from "../../lib/format";
import { SEVERITY } from "../../lib/scale";
import type { Analysis, EntrySummary } from "../../lib/types";
import { Icon } from "./Icon";

export type ViewMode = "compare" | "rgb" | "ndvi";

export function imageUrls(entry: EntrySummary) {
  return {
    rgb: `/api/entries/${entry.id}/preview/rgb.jpg?v=${entry.version}`,
    ndvi: `/api/entries/${entry.id}/preview/ndvi.png?v=${entry.version}`,
  };
}

export function ImageCompare({
  entry,
  analysis,
  selectedHotspot,
  onSelectHotspot,
}: {
  entry: EntrySummary;
  analysis: Analysis | null;
  selectedHotspot: number | null;
  onSelectHotspot: (id: number) => void;
}) {
  const [mode, setMode] = useState<ViewMode>("compare");
  const [split, setSplit] = useState(0.5);
  const [natural, setNatural] = useState<number | null>(null);
  const [loaded, setLoaded] = useState({ rgb: false, ndvi: false });
  const [failed, setFailed] = useState(false);
  const [showHotspots, setShowHotspots] = useState(true);
  const stage = useRef<HTMLDivElement>(null);
  const dragging = useRef(false);

  useEffect(() => {
    setLoaded({ rgb: false, ndvi: false });
    setFailed(false);
    setNatural(null);
  }, [entry.id, entry.version]);

  const urls = imageUrls(entry);
  const aspect = entry.crop ? entry.crop.width / entry.crop.height : natural ?? 4 / 3;
  const hotspots = showHotspots ? analysis?.hotspots ?? [] : [];

  const move = useCallback((clientX: number) => {
    const rect = stage.current?.getBoundingClientRect();
    if (rect) setSplit(Math.min(1, Math.max(0, (clientX - rect.left) / rect.width)));
  }, []);

  const clip = mode === "compare" ? `inset(0 0 0 ${split * 100}%)` : mode === "ndvi" ? "inset(0 0 0 0)" : "inset(0 0 0 100%)";

  if (entry.status === "processing" || entry.status === "error") {
    return (
      <div className="viewer-placeholder" style={{ aspectRatio: "4 / 3" }}>
        {entry.status === "processing" ? (
          <><span className="spinner large" /><strong>Processando na estação</strong><span>Alinhamento, NDVI e detecção de focos</span></>
        ) : (
          <><Icon name="alert" size={28} /><strong>Falha no processamento</strong><span>{entry.error}</span><code>python reprocess.py {entry.id.slice(0, 8)}…</code></>
        )}
      </div>
    );
  }

  return (
    <div className="viewer">
      <div
        ref={stage}
        className={`viewer-stage mode-${mode}`}
        style={{ aspectRatio: String(aspect) }}
        onPointerDown={(event) => {
          if (mode !== "compare" || (event.target as HTMLElement).closest(".hotspot-badge")) return;
          dragging.current = true;
          (event.currentTarget as HTMLElement).setPointerCapture(event.pointerId);
          move(event.clientX);
        }}
        onPointerMove={(event) => dragging.current && move(event.clientX)}
        onPointerUp={() => (dragging.current = false)}
        onKeyDown={(event) => {
          if (mode !== "compare") return;
          if (event.key === "ArrowLeft") setSplit((s) => Math.max(0, s - 0.05));
          if (event.key === "ArrowRight") setSplit((s) => Math.min(1, s + 0.05));
        }}
        tabIndex={mode === "compare" ? 0 : -1}
        role={mode === "compare" ? "slider" : undefined}
        aria-label={mode === "compare" ? "Comparação RGB e NDVI" : undefined}
        aria-valuenow={mode === "compare" ? Math.round(split * 100) : undefined}
      >
        <img
          src={urls.rgb}
          alt="Imagem RGB da captura"
          className={loaded.rgb ? "ready" : ""}
          draggable={false}
          onLoad={(event) => {
            setLoaded((l) => ({ ...l, rgb: true }));
            setNatural(event.currentTarget.naturalWidth / event.currentTarget.naturalHeight);
          }}
          onError={() => setFailed(true)}
        />
        <img
          src={urls.ndvi}
          alt="NDVI da captura em escala divergente"
          className={`ndvi-layer ${loaded.ndvi ? "ready" : ""}`}
          style={{ clipPath: clip }}
          draggable={false}
          onLoad={() => setLoaded((l) => ({ ...l, ndvi: true }))}
          onError={() => setFailed(true)}
        />

        {hotspots.length > 0 && (
          <svg className="hotspot-shapes" viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
            {hotspots.map((h) => (
              <polygon
                key={h.id}
                points={h.polygon.map(([x, y]) => `${x * 100},${y * 100}`).join(" ")}
                className={selectedHotspot === h.id ? "selected" : ""}
                style={{ ["--sev" as string]: SEVERITY[h.severity].color }}
              />
            ))}
          </svg>
        )}
        {hotspots.map((h) => {
          const w = entry.crop?.width ?? 1;
          const hh = entry.crop?.height ?? 1;
          return (
            <button
              key={h.id}
              className={`hotspot-badge ${selectedHotspot === h.id ? "selected" : ""}`}
              style={{ left: `${(h.centroid_px[0] / w) * 100}%`, top: `${(h.centroid_px[1] / hh) * 100}%`, ["--sev" as string]: SEVERITY[h.severity].color }}
              onClick={() => onSelectHotspot(h.id)}
              title={`Foco ${h.id} · queda de NDVI ${fmt.number(h.ndvi_drop, 2)}`}
            >
              {h.id}
            </button>
          );
        })}

        {mode === "compare" && (
          <div className="split-handle" style={{ left: `${split * 100}%` }} aria-hidden="true">
            <span><Icon name="left" size={12} stroke={2.4} /><Icon name="right" size={12} stroke={2.4} /></span>
          </div>
        )}
        {mode === "compare" && (
          <>
            <span className="stage-label left">RGB</span>
            <span className="stage-label right">NDVI</span>
          </>
        )}
        {(!loaded.rgb || !loaded.ndvi) && !failed && <div className="stage-loading"><span className="spinner" /></div>}
        {failed && <div className="stage-loading"><Icon name="image" size={22} /> Imagem indisponível</div>}
      </div>

      <div className="viewer-toolbar">
        <div className="segmented" role="tablist" aria-label="Modo de visualização">
          {(["compare", "rgb", "ndvi"] as ViewMode[]).map((m) => (
            <button key={m} role="tab" aria-selected={mode === m} className={mode === m ? "active" : ""} onClick={() => setMode(m)}>
              {m === "compare" ? "Comparar" : m === "rgb" ? "RGB" : "NDVI"}
            </button>
          ))}
        </div>
        {(analysis?.hotspots.length ?? 0) > 0 && (
          <button className={`toggle ${showHotspots ? "on" : ""}`} onClick={() => setShowHotspots((v) => !v)} aria-pressed={showHotspots}>
            <Icon name="target" size={14} /> Focos
          </button>
        )}
      </div>
    </div>
  );
}
