import { Injectable } from '@angular/core';

/// Persists the sidebar's drag-to-reorder order per browser (localStorage),
/// mirroring the Theme/Debug toggles — no backend, no per-user sync. The order
/// is a list of menu ids; `apply()` reorders a menu array by it and APPENDS any
/// id not in the saved list (so a newly code-added page is never hidden).
@Injectable({ providedIn: 'root' })
export class NavOrderService {
  private readonly KEY = 'iot-nav-order';

  private read(): string[] {
    try {
      const v = JSON.parse(localStorage.getItem(this.KEY) || '[]');
      return Array.isArray(v) ? v.filter((x) => typeof x === 'string') : [];
    } catch { return []; }
  }

  save(ids: string[]): void {
    try { localStorage.setItem(this.KEY, JSON.stringify(ids)); } catch { /* private mode */ }
  }

  reset(): void {
    try { localStorage.removeItem(this.KEY); } catch { /* private mode */ }
  }

  /// Reorder `items` by the saved id order; ids present in `items` but not in
  /// the saved order keep their original relative position at the end.
  apply<T extends { id: string }>(items: T[]): T[] {
    const order = this.read();
    if (!order.length) return items;
    const byId = new Map(items.map((i) => [i.id, i]));
    const out: T[] = [];
    for (const id of order) {
      const it = byId.get(id);
      if (it) { out.push(it); byId.delete(id); }
    }
    for (const it of items) if (byId.has(it.id)) out.push(it);   // appended
    return out;
  }
}
