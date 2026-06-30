import { Component, OnInit } from '@angular/core';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';

/// One cloud.audit.log record (newest-first array written by iot-httpd, P5c).
interface AuditRow {
  ts: number;        // unix epoch seconds
  actor: string;     // session username, or "system"
  tenant: string;    // actor's tenant ("*" = platform operator)
  action: string;    // e.g. "device.provision", "tenant.update"
  target?: string;   // serial / tenant id acted on
  detail?: string;
}

/// Operator audit log (P5c). Read-only view of cloud.audit.log — who did what,
/// when. iot-httpd is the sole writer (appends in the db/set handler for tenant
/// + device lifecycle actions); this page just renders the capped array.
@Component({
  selector: 'app-audit',
  template: `
    <div class="page">
      <h3>Audit Log
        <button class="btn btn-sm btn-link" (click)="reload()" [disabled]="loading">Refresh</button>
      </h3>

      <ng-container *ngIf="isAdmin; else noAccess">
        <p class="hint">
          Tenant &amp; device lifecycle actions (provision, deprovision, tenant
          changes), newest first. Recorded by the cloud at the point of action.
        </p>
        <clr-datagrid [clrDgLoading]="loading" style="margin-top:12px;">
          <clr-dg-column>Time</clr-dg-column>
          <clr-dg-column>Actor</clr-dg-column>
          <clr-dg-column>Tenant</clr-dg-column>
          <clr-dg-column>Action</clr-dg-column>
          <clr-dg-column>Target</clr-dg-column>

          <clr-dg-row *clrDgItems="let e of rows">
            <clr-dg-cell>{{ fmt(e.ts) }}</clr-dg-cell>
            <clr-dg-cell>{{ e.actor || '—' }}</clr-dg-cell>
            <clr-dg-cell><code>{{ e.tenant || 'default' }}</code></clr-dg-cell>
            <clr-dg-cell><span class="action">{{ e.action }}</span></clr-dg-cell>
            <clr-dg-cell>
              <code *ngIf="e.target">{{ e.target }}</code>
              <span *ngIf="!e.target" class="hint">—</span>
              <span *ngIf="e.detail" class="hint"> ({{ e.detail }})</span>
            </clr-dg-cell>
          </clr-dg-row>

          <clr-dg-footer>{{ rows.length }} event{{ rows.length===1?'':'s' }}</clr-dg-footer>
        </clr-datagrid>
      </ng-container>

      <ng-template #noAccess>
        <p class="hint">You need Admin access to view the audit log.</p>
      </ng-template>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 12px 0; font-size: 16px; font-weight: 600; }
    .hint { color: #888; font-size: 12px; }
    .action { font-family: monospace; font-size: 12px; color: #0072a3; }
  `]
})
export class AuditComponent implements OnInit {
  rows: AuditRow[] = [];
  loading = false;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private session: SessionService) {}

  ngOnInit(): void { if (this.isAdmin) this.reload(); }

  fmt(ts: number): string {
    if (!ts) return '—';
    try { return new Date(ts * 1000).toLocaleString(); } catch { return String(ts); }
  }

  reload(): void {
    this.loading = true;
    this.http.dbGet(['cloud.audit.log']).subscribe({
      next: (r) => {
        this.loading = false;
        if (r.ok && r.data) {
          try {
            const arr = JSON.parse(String(r.data['cloud.audit.log'] ?? '[]'));
            this.rows = Array.isArray(arr) ? arr : [];
          } catch { this.rows = []; }
        }
      },
      error: () => { this.loading = false; }
    });
  }
}
