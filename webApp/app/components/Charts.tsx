"use client";

import { useState } from "react";
import { fmt } from "../../lib/format";
import { ndviColor } from "../../lib/scale";

// ===== Histograma do NDVI de uma foto =====
// Cada barra recebe a cor do próprio valor de NDVI (a barra é a legenda).

export function NdviHistogram({
  edges,
  counts,
  median,
  threshold,
}: {
  edges: number[];
  counts: number[];
  median: number | null;
  threshold: number | null;
}) {
  const [hover, setHover] = useState<number | null>(null);
  const W = 320, H = 132, top = 10, bottom = 22;
  const plotH = H - top - bottom;
  const total = counts.reduce((a, b) => a + b, 0) || 1;
  const max = Math.max(...counts, 1);
  const bw = W / counts.length;
  const x = (v: number) => ((v + 1) / 2) * W;

  return (
    <figure className="chart">
      <svg viewBox={`0 0 ${W} ${H}`} className="chart-svg" role="img" aria-label="Distribuição dos valores de NDVI da foto">
        {[0.5, 1].map((f) => (
          <line key={f} x1={0} x2={W} y1={top + plotH * (1 - f)} y2={top + plotH * (1 - f)} className="grid" />
        ))}
        {counts.map((c, i) => {
          const h = Math.max(c ? 2 : 0, (c / max) * plotH);
          const center = (edges[i] + edges[i + 1]) / 2;
          const y = top + plotH - h;
          const r = Math.min(2, h / 2, (bw - 2) / 2);
          return (
            <g key={i} onMouseEnter={() => setHover(i)} onMouseLeave={() => setHover(null)}>
              <rect x={i * bw} y={top} width={bw} height={plotH} fill="transparent" />
              {h > 0 && (
                <path
                  d={`M${i * bw + 1},${top + plotH} V${y + r} Q${i * bw + 1},${y} ${i * bw + 1 + r},${y} H${(i + 1) * bw - 1 - r} Q${(i + 1) * bw - 1},${y} ${(i + 1) * bw - 1},${y + r} V${top + plotH} Z`}
                  fill={ndviColor(center)}
                  opacity={hover === null || hover === i ? 1 : 0.45}
                />
              )}
            </g>
          );
        })}
        <line x1={0} x2={W} y1={top + plotH} y2={top + plotH} className="axis" />
        {threshold !== null && (
          <g className="marker">
            <line x1={x(threshold)} x2={x(threshold)} y1={top - 4} y2={top + plotH} strokeDasharray="3 3" />
          </g>
        )}
        {median !== null && (
          <g className="marker marker-strong">
            <line x1={x(median)} x2={x(median)} y1={top - 4} y2={top + plotH} />
            <circle cx={x(median)} cy={top - 4} r={3} />
          </g>
        )}
        {[-1, -0.5, 0, 0.5, 1].map((v) => (
          <text key={v} x={Math.min(W - 8, Math.max(8, x(v)))} y={H - 6} className="tick" textAnchor="middle">
            {v === 0 ? "0" : fmt.signed(v, 1)}
          </text>
        ))}
      </svg>
      {hover !== null && (
        <div className="chart-tooltip" style={{ left: `${((hover + 0.5) / counts.length) * 100}%` }}>
          <strong>{fmt.signed(edges[hover])} a {fmt.signed(edges[hover + 1])}</strong>
          <span>{fmt.percent(counts[hover] / total, 1)} dos pixels</span>
        </div>
      )}
      <figcaption className="chart-legend">
        <span><i className="legend-line solid" />Mediana {fmt.signed(median)}</span>
        {threshold !== null && <span><i className="legend-line dashed" />Limiar de vegetação {fmt.signed(threshold)}</span>}
      </figcaption>
    </figure>
  );
}

// ===== NDVI médio ao longo da missão =====

export function NdviSeries({
  points,
  selected,
  onSelect,
}: {
  points: { id: string; sequence: number; value: number; hotspots: number }[];
  selected: string | null;
  onSelect: (id: string) => void;
}) {
  const [hover, setHover] = useState<number | null>(null);
  if (points.length < 2) return <p className="chart-empty">O gráfico aparece a partir de duas fotos processadas.</p>;

  const W = 320, H = 140, left = 30, right = 8, top = 12, bottom = 22;
  const values = points.map((p) => p.value);
  let lo = Math.min(...values), hi = Math.max(...values);
  const pad = Math.max(0.05, (hi - lo) * 0.2);
  lo = Math.max(-1, lo - pad);
  hi = Math.min(1, hi + pad);
  const px = (i: number) => left + (i / (points.length - 1)) * (W - left - right);
  const py = (v: number) => top + (1 - (v - lo) / (hi - lo || 1)) * (H - top - bottom);
  const ticks = [lo, (lo + hi) / 2, hi];
  const path = points.map((p, i) => `${i ? "L" : "M"}${px(i)},${py(p.value)}`).join(" ");
  const area = `${path} L${px(points.length - 1)},${H - bottom} L${px(0)},${H - bottom} Z`;
  const active = hover ?? points.findIndex((p) => p.id === selected);

  return (
    <figure className="chart">
      <svg
        viewBox={`0 0 ${W} ${H}`}
        className="chart-svg"
        role="img"
        aria-label="NDVI médio de cada foto ao longo da missão"
        onMouseLeave={() => setHover(null)}
        onMouseMove={(event) => {
          const rect = event.currentTarget.getBoundingClientRect();
          const sx = ((event.clientX - rect.left) / rect.width) * W;
          const i = Math.round(((sx - left) / (W - left - right)) * (points.length - 1));
          setHover(Math.max(0, Math.min(points.length - 1, i)));
        }}
        onClick={() => hover !== null && onSelect(points[hover].id)}
      >
        <defs>
          <linearGradient id="series-fill" x1="0" x2="0" y1="0" y2="1">
            <stop offset="0%" stopColor="var(--accent)" stopOpacity="0.22" />
            <stop offset="100%" stopColor="var(--accent)" stopOpacity="0" />
          </linearGradient>
        </defs>
        {ticks.map((t, i) => (
          <g key={i}>
            <line x1={left} x2={W - right} y1={py(t)} y2={py(t)} className="grid" />
            <text x={left - 6} y={py(t) + 3} className="tick" textAnchor="end">{fmt.signed(t, 2)}</text>
          </g>
        ))}
        <path d={area} fill="url(#series-fill)" />
        <path d={path} className="series-line" />
        {active >= 0 && <line x1={px(active)} x2={px(active)} y1={top} y2={H - bottom} className="crosshair" />}
        {points.map((p, i) => (
          <circle
            key={p.id}
            cx={px(i)}
            cy={py(p.value)}
            r={i === active ? 5 : p.hotspots ? 3.5 : 2.5}
            className={`series-dot ${p.hotspots ? "has-hotspots" : ""} ${i === active ? "active" : ""}`}
          />
        ))}
        {[0, points.length - 1].map((i) => (
          <text key={i} x={px(i)} y={H - 6} className="tick" textAnchor={i ? "end" : "start"}>Foto {points[i].sequence}</text>
        ))}
      </svg>
      {hover !== null && (
        <div className="chart-tooltip" style={{ left: `${(px(hover) / W) * 100}%` }}>
          <strong>Foto {points[hover].sequence}</strong>
          <span>NDVI médio {fmt.signed(points[hover].value)}</span>
          <span>{points[hover].hotspots} foco{points[hover].hotspots === 1 ? "" : "s"}</span>
        </div>
      )}
      <figcaption className="chart-legend">
        <span><i className="legend-dot" />NDVI médio por foto</span>
        <span><i className="legend-dot ring" />Foto com focos</span>
      </figcaption>
    </figure>
  );
}
