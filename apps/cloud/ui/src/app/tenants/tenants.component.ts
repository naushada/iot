import { Component, OnInit } from '@angular/core';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';

/// One row of the cloud.tenants JSON array. Keys use the dotted ds convention.
interface TenantRow {
  id: string;
  name?: string;
  'vpn.subnet'?: string;     // auto-assigned by iot-cloudd (P4a) from the pool
  'dm.uri'?: string;         // optional per-tenant DM URI override
  'max.devices'?: number;    // optional per-tenant quota (P5a; 0/absent = unlimited)
  status?: string;           // "active" | "suspended"
}

/// Multi-tenant operator console (P4c). Platform-operator CRUD over the
/// cloud.tenants registry, driven through the generic /api/v1/db surface — the
/// same pattern as every other cloud-ui config page. iot-cloudd watches
/// cloud.tenants and auto-carves a non-overlapping /24 (P4a) for any tenant
/// lacking one, so the add form only needs id + name (+ optional quota/DM URI);
/// vpn.subnet is shown read-only once assigned.
@Component({
  selector: 'app-tenants',
  template: `
    <div class="page">
      <h3>Tenants</h3>

      <ng-container *ngIf="isAdmin; else noAccess">
        <!-- Create / update -->
        <div class="form-grid" style="align-items:end;">
          <clr-input-container>
            <label>Tenant ID</label>
            <input clrInput [(ngModel)]="newId" [readonly]="editing" placeholder="acme" />
            <clr-control-helper *dsDebug><app-ds-hint key="cloud.tenants"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>Display name</label>
            <input clrInput [(ngModel)]="newName" placeholder="Acme Corp" />
          </clr-input-container>
          <clr-input-container>
            <label>Max devices</label>
            <input clrInput type="number" min="0" [(ngModel)]="newMax" placeholder="0 = unlimited" />
            <clr-control-helper *dsDebug><app-ds-hint key="cloud.tenants (max.devices)"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-input-container>
            <label>DM URI (optional)</label>
            <input clrInput [(ngModel)]="newDmUri" placeholder="coaps://cloud.example:5683" />
          </clr-input-container>
          <div class="btn-cell">
            <button class="btn btn-primary" (click)="save()" [disabled]="saving">
              {{ saving ? 'Saving…' : (editing ? 'Update' : 'Add tenant') }}
            </button>
            <button *ngIf="editing" class="btn btn-link" (click)="cancelEdit()">Cancel</button>
          </div>
        </div>
        <p class="hint">
          The VPN subnet is auto-assigned by the cloud from the tenant pool — no
          need to enter it. <code>default</code> is the built-in single-tenant
          bucket and can't be deleted.
        </p>

        <!-- Tenant list -->
        <clr-datagrid [clrDgLoading]="loading" style="margin-top:20px;">
          <clr-dg-column>ID</clr-dg-column>
          <clr-dg-column>Name</clr-dg-column>
          <clr-dg-column>VPN subnet</clr-dg-column>
          <clr-dg-column>Max devices</clr-dg-column>
          <clr-dg-column>Status</clr-dg-column>
          <clr-dg-column>Actions</clr-dg-column>

          <clr-dg-row *clrDgItems="let t of tenants">
            <clr-dg-cell><code>{{ t.id }}</code></clr-dg-cell>
            <clr-dg-cell>{{ t.name || '—' }}</clr-dg-cell>
            <clr-dg-cell>{{ t['vpn.subnet'] || '(assigning…)' }}</clr-dg-cell>
            <clr-dg-cell>{{ t['max.devices'] ? t['max.devices'] : 'unlimited' }}</clr-dg-cell>
            <clr-dg-cell>
              <app-status-badge [label]="t.status || 'active'"
                [state]="(t.status || 'active')==='active' ? 'connected' : 'idle'"></app-status-badge>
            </clr-dg-cell>
            <clr-dg-cell>
              <button class="btn btn-sm btn-outline" (click)="edit(t)">Edit</button>
              <button class="btn btn-sm btn-outline" (click)="toggleStatus(t)">
                {{ (t.status || 'active')==='active' ? 'Suspend' : 'Activate' }}
              </button>
              <button class="btn btn-sm btn-danger" *ngIf="t.id !== 'default'"
                      (click)="remove(t.id)">Delete</button>
              <span *ngIf="t.id === 'default'" class="hint">built-in</span>
            </clr-dg-cell>
          </clr-dg-row>

          <clr-dg-footer>{{ tenants.length }} tenant{{ tenants.length===1?'':'s' }}</clr-dg-footer>
        </clr-datagrid>
      </ng-container>

      <ng-template #noAccess>
        <p class="hint">You need Admin access to manage tenants.</p>
      </ng-template>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 20px 0; font-size: 16px; font-weight: 600; }
    .hint { color: #888; font-size: 12px; margin-top: 8px; }
    .btn-cell { display: flex; align-items: flex-end; gap: 8px; }
    .btn-cell .btn-primary { white-space: nowrap; }
    .btn-sm { margin-right: 6px; }
  `]
})
export class TenantsComponent implements OnInit {
  tenants: TenantRow[] = [];
  newId = '';
  newName = '';
  newMax: number | null = null;
  newDmUri = '';
  editing = false;
  loading = false;
  saving = false;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private session: SessionService,
              private toast: ToastService) {}

  ngOnInit(): void { if (this.isAdmin) this.reload(); }

  reload(): void {
    this.loading = true;
    this.http.dbGet(['cloud.tenants']).subscribe({
      next: (r) => {
        this.loading = false;
        if (r.ok && r.data) {
          try {
            const arr = JSON.parse(String(r.data['cloud.tenants'] ?? '[]'));
            this.tenants = Array.isArray(arr) ? arr : [];
          } catch { this.tenants = []; }
        }
      },
      error: () => { this.loading = false; }
    });
  }

  edit(t: TenantRow): void {
    this.editing = true;
    this.newId = t.id;
    this.newName = t.name || '';
    this.newMax = t['max.devices'] ?? null;
    this.newDmUri = t['dm.uri'] || '';
  }

  cancelEdit(): void {
    this.editing = false;
    this.newId = ''; this.newName = ''; this.newMax = null; this.newDmUri = '';
  }

  save(): void {
    const id = this.newId.trim().toLowerCase();
    if (!/^[a-z0-9-]{3,32}$/.test(id)) {
      this.toast.error('Tenant ID must be 3–32 chars of [a-z0-9-]'); return;
    }
    // Merge into the array: update an existing row in place (preserving its
    // auto-assigned vpn.subnet / port range), else append a fresh one.
    const next = this.tenants.map((t) => ({ ...t }));
    const idx = next.findIndex((t) => t.id === id);
    if (!this.editing && idx >= 0) {
      this.toast.error('Tenant ' + id + ' already exists'); return;
    }
    const row: TenantRow = idx >= 0 ? next[idx] : { id, status: 'active' };
    row.name = this.newName.trim();
    if (this.newMax && this.newMax > 0) row['max.devices'] = Number(this.newMax);
    else delete row['max.devices'];
    if (this.newDmUri.trim()) row['dm.uri'] = this.newDmUri.trim();
    else delete row['dm.uri'];
    if (idx < 0) next.push(row);
    this.commit(next, (this.editing ? 'Updated ' : 'Added ') + id);
  }

  toggleStatus(t: TenantRow): void {
    const next = this.tenants.map((x) =>
      x.id === t.id ? { ...x, status: (x.status || 'active') === 'active' ? 'suspended' : 'active' } : x);
    this.commit(next, t.id + ' ' + ((t.status || 'active') === 'active' ? 'suspended' : 'activated'));
  }

  remove(id: string): void {
    if (id === 'default') return;
    const next = this.tenants.filter((t) => t.id !== id);
    this.commit(next, 'Deleted ' + id);
  }

  /// Write the whole array back; iot-cloudd's cloud.tenants watch reconciles
  /// (subnet carve, nft isolation rebuild). Reload to pick up the assigned subnet.
  private commit(next: TenantRow[], msg: string): void {
    this.saving = true;
    this.http.dbSet([{ key: 'cloud.tenants', value: JSON.stringify(next) }]).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) { this.toast.success(msg); this.cancelEdit(); this.reload(); }
        else this.toast.error(r.err || 'Save failed');
      },
      error: (e) => { this.saving = false; this.toast.error(e?.error?.err || 'Save failed'); }
    });
  }
}
