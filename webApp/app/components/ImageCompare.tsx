"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import { fmt } from "../../lib/format";
import { SEVERITY } from "../../lib/scale";
import type { Analysis, EntrySummary } from "../../lib/types";
import { Icon } from "./Icon";

export type ViewMode = "compare" | "rgb" | "ndvi";

const MAX_ZOOM = 8;
const HANDLE_GRAB_PX = 22;

export function imageUrls(entry: EntrySummary) {
  return {
    rgb: `/api/entries/${entry.id}/preview/rgb.jpg?v=${entry.version}`,
    ndvi: `/api/entries/${entry.id}/preview/ndvi.png?v=${entry.version}`,
  };
}

type View = { s: number; tx: number; ty: number };
const IDENTITY: View = { s: 1, tx: 0, ty: 0 };

// Mantém a imagem cobrindo o palco (sem bordas vazias ao arrastar).
function clampView(v: View, w: number, h: number): View {
  const s = Math.min(MAX_ZOOM, Math.max(1, v.s));
  return { s, tx: Math.min(0, Math.max(w - s * w, v.tx)), ty: Math.min(0, Math.max(h - s * h, v.ty)) };
}

export function ImageCompare({
  entry,
  analysis,
  selectedHotspot,
  onSelectHotspot,
  expanded = false,
  onExpand,
  initialMode = "compare",
}: {
  entry: EntrySummary;
  analysis: Analysis | null;
  selectedHotspot: number | null;
  onSelectHotspot: (id: number) => void;
  /** Modo ampliado: zoom com a roda do mouse e arrasto para mover. */
  expanded?: boolean;
  onExpand?: () => void;
  initialMode?: ViewMode;
}) {
  const [mode, setMode] = useState<ViewMode>(initialMode);
  const [split, setSplit] = useState(0.5);
  const [natural, setNatural] = useState<number | null>(null);
  const [loaded, setLoaded] = useState({ rgb: false, ndvi: false });
  const [failed, setFailed] = useState(false);
  const [showHotspots, setShowHotspots] = useState(true);
  const [view, setView] = useState<View>(IDENTITY);
  const [size, setSize] = useState({ w: 1, h: 1 });
  const stage = useRef<HTMLDivElement>(null);
  const drag = useRef<null | { kind: "split" } | { kind: "pan"; x: number; y: number; tx: number; ty: number; moved: boolean }>(null);

  useEffect(() => {
    setLoaded({ rgb: false, ndvi: false });
    setFailed(false);
    setNatural(null);
    setView(IDENTITY);
  }, [entry.id, entry.version]);

  useEffect(() => {
    const el = stage.current;
    if (!el) return;
    const ro = new ResizeObserver(([e]) => setSize({ w: e.contentRect.width || 1, h: e.contentRect.height || 1 }));
    ro.observe(el);
    return () => ro.disconnect();
  }, [entry.status]);

  // Zoom na posição do cursor (listener não passivo para poder cancelar o scroll).
  useEffect(() => {
    const el = stage.current;
    if (!el || !expanded) return;
    const wheel = (event: WheelEvent) => {
      event.preventDefault();
      const rect = el.getBoundingClientRect();
      const cx = event.clientX - rect.left;
      const cy = event.clientY - rect.top;
      setView((v) => {
        const s = Math.min(MAX_ZOOM, Math.max(1, v.s * Math.exp(-event.deltaY * 0.0015)));
        const k = s / v.s;
        return clampView({ s, tx: cx - (cx - v.tx) * k, ty: cy - (cy - v.ty) * k }, rect.width, rect.height);
      });
    };
    el.addEventListener("wheel", wheel, { passive: false });
    return () => el.removeEventListener("wheel", wheel);
  }, [expanded, entry.status]);

  const zoomBy = (factor: number) =>
    setView((v) => {
      const cx = size.w / 2, cy = size.h / 2;
      const s = Math.min(MAX_ZOOM, Math.max(1, v.s * factor));
      return clampView({ s, tx: cx - (cx - v.tx) * (s / v.s), ty: cy - (cy - v.ty) * (s / v.s) }, size.w, size.h);
    });

  const urls = imageUrls(entry);
  const aspect = entry.crop ? entry.crop.width / entry.crop.height : natural ?? 4 / 3;
  const hotspots = showHotspots ? analysis?.hotspots ?? [] : [];
  const zoomed = view.s > 1.001;

  const setSplitAt = useCallback((clientX: number) => {
    const rect = stage.current?.getBoundingClientRect();
    if (rect) setSplit(Math.min(1, Math.max(0, (clientX - rect.left) / rect.width)));
  }, []);

  // A divisória fica na tela; o recorte da camada NDVI é na imagem (que pode estar ampliada).
  const splitOnImage = (split * size.w - view.tx) / (view.s * size.w);
  const clip =
    mode === "compare" ? `inset(0 0 0 ${Math.min(1, Math.max(0, splitOnImage)) * 100}%)` : mode === "ndvi" ? "inset(0 0 0 0)" : "inset(0 0 0 100%)";

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

  const w = entry.crop?.width ?? 1;
  const hh = entry.crop?.height ?? 1;

  return (
    <div className={`viewer ${expanded ? "viewer-expanded" : ""}`} style={expanded ? { ["--fit-w" as string]: `min(100%, calc((100dvh - 210px) * ${aspect}))` } : undefined}>
      <div
        ref={stage}
        className={`viewer-stage mode-${mode} ${zoomed ? "zoomed" : ""} ${!expanded && onExpand && mode !== "compare" ? "can-expand" : ""}`}
        style={{ aspectRatio: String(aspect) }}
        onPointerDown={(event) => {
          if ((event.target as HTMLElement).closest(".hotspot-badge, .stage-btn")) return;
          const rect = event.currentTarget.getBoundingClientRect();
          const nearHandle = mode === "compare" && Math.abs(event.clientX - rect.left - split * rect.width) < HANDLE_GRAB_PX;
          if (mode === "compare" && (!zoomed || nearHandle)) {
            drag.current = { kind: "split" };
            setSplitAt(event.clientX);
          } else {
            drag.current = { kind: "pan", x: event.clientX, y: event.clientY, tx: view.tx, ty: view.ty, moved: false };
          }
          event.currentTarget.setPointerCapture(event.pointerId);
        }}
        onPointerMove={(event) => {
          const d = drag.current;
          if (!d) return;
          if (d.kind === "split") return setSplitAt(event.clientX);
          const dx = event.clientX - d.x, dy = event.clientY - d.y;
          if (Math.abs(dx) + Math.abs(dy) > 3) d.moved = true;
          if (zoomed) setView((v) => clampView({ ...v, tx: d.tx + dx, ty: d.ty + dy }, size.w, size.h));
        }}
        onPointerUp={() => {
          const d = drag.current;
          drag.current = null;
          // Clique simples (sem arrastar) fora do modo comparar abre o visualizador ampliado.
          if (d?.kind === "pan" && !d.moved && !expanded && onExpand) onExpand();
        }}
        onDoubleClick={(event) => {
          if ((event.target as HTMLElement).closest(".hotspot-badge, .stage-btn")) return;
          if (!expanded) onExpand?.();
          else setView(IDENTITY);
        }}
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
        <div className="viewer-canvas" style={{ transform: `translate(${view.tx}px, ${view.ty}px) scale(${view.s})` }}>
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
        </div>

        {/* Marcadores fora da camada ampliada: mantêm o tamanho com qualquer zoom. */}
        {hotspots.map((h) => {
          const left = (view.tx + view.s * (h.centroid_px[0] / w) * size.w) / size.w;
          const top = (view.ty + view.s * (h.centroid_px[1] / hh) * size.h) / size.h;
          if (left < -0.02 || left > 1.02 || top < -0.02 || top > 1.1) return null;
          return (
            <button
              key={h.id}
              className={`hotspot-badge ${selectedHotspot === h.id ? "selected" : ""}`}
              style={{ left: `${left * 100}%`, top: `${top * 100}%`, ["--sev" as string]: SEVERITY[h.severity].color }}
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
        {!expanded && onExpand && (
          <button className="stage-btn" onClick={onExpand} aria-label="Ampliar foto" title="Ampliar (duplo clique na foto)">
            <Icon name="expand" size={15} />
          </button>
        )}
        {expanded && zoomed && <span className="zoom-chip mono">{fmt.number(view.s, 1)}×</span>}
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
        <div className="viewer-actions">
          {expanded && (
            <div className="segmented zoom-controls" aria-label="Zoom">
              <button onClick={() => zoomBy(1 / 1.5)} disabled={!zoomed} aria-label="Diminuir zoom">−</button>
              <button onClick={() => setView(IDENTITY)} disabled={!zoomed} className="mono">{fmt.number(view.s, 1)}×</button>
              <button onClick={() => zoomBy(1.5)} disabled={view.s >= MAX_ZOOM} aria-label="Aumentar zoom">+</button>
            </div>
          )}
          {(analysis?.hotspots.length ?? 0) > 0 && (
            <button className={`toggle ${showHotspots ? "on" : ""}`} onClick={() => setShowHotspots((v) => !v)} aria-pressed={showHotspots}>
              <Icon name="target" size={14} /> Focos
            </button>
          )}
        </div>
      </div>
    </div>
  );
}
