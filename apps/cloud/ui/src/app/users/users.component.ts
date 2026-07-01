import { Component, OnInit } from '@angular/core';
import { HttpsvcService } from '../../common/httpsvc.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';
import { UserAccount } from '../../common/app-globals';

@Component({
  selector: 'app-users',
  template: `
    <div class="page">
      <h3>Users</h3>

      <ng-container *ngIf="isAdmin; else noAccess">
        <!-- Create / update -->
        <div class="form-grid" style="align-items:end;">
          <clr-input-container>
            <label>User ID</label>
            <input clrInput [(ngModel)]="newId" placeholder="alice" />
            <clr-control-helper *dsDebug><app-ds-hint key="auth.users.accounts (via /api/v1/users)"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <clr-password-container>
            <label>Password</label>
            <input clrPassword type="password" [(ngModel)]="newPassword" placeholder="••••••••" />
            <clr-control-helper *dsDebug><app-ds-hint key="auth.users.accounts (SHA-256, write-only)"></app-ds-hint></clr-control-helper>
          </clr-password-container>
          <clr-select-container>
            <label>Access</label>
            <select clrSelect [(ngModel)]="newAccess">
              <option *ngFor="let a of accessLevels" [value]="a">{{ a }}</option>
            </select>
            <clr-control-helper *dsDebug><app-ds-hint key="auth.users.accounts"></app-ds-hint></clr-control-helper>
          </clr-select-container>
          <clr-select-container>
            <label>Tenant</label>
            <select clrSelect [(ngModel)]="newTenant">
              <option *ngFor="let t of tenantIds" [value]="t">{{ t === '*' ? '* (platform operator)' : t }}</option>
            </select>
            <clr-control-helper *dsDebug><app-ds-hint key="auth.users.accounts (tenant)"></app-ds-hint></clr-control-helper>
          </clr-select-container>
          <div class="btn-cell">
            <button class="btn btn-primary" (click)="create()" [disabled]="saving">
              {{ saving ? 'Saving…' : 'Add / Update' }}
            </button>
          </div>
        </div>
        <p class="hint">Existing ID updates that user's password &amp; access. The built-in <code>admin</code> can't be created or deleted here.</p>

        <!-- User list -->
        <clr-datagrid [clrDgLoading]="loading" style="margin-top:20px;">
          <clr-dg-column>User ID</clr-dg-column>
          <clr-dg-column>Access</clr-dg-column>
          <clr-dg-column>Tenant</clr-dg-column>
          <clr-dg-column>Actions</clr-dg-column>

          <clr-dg-row *clrDgItems="let u of users">
            <clr-dg-cell>{{ u.id }}</clr-dg-cell>
            <clr-dg-cell>
              <app-status-badge [label]="u.access" [state]="u.access==='Admin' ? 'connected' : 'idle'"></app-status-badge>
            </clr-dg-cell>
            <clr-dg-cell>
              <code>{{ u.tenant === '*' ? '* (operator)' : (u.tenant || 'default') }}</code>
            </clr-dg-cell>
            <clr-dg-cell>
              <button class="btn btn-sm btn-danger" *ngIf="u.id !== 'admin'" (click)="remove(u.id)">Delete</button>
              <span *ngIf="u.id === 'admin'" class="hint">built-in</span>
            </clr-dg-cell>
          </clr-dg-row>

          <clr-dg-footer>{{ users.length }} user{{ users.length===1?'':'s' }}</clr-dg-footer>
        </clr-datagrid>
      </ng-container>

      <ng-template #noAccess>
        <p class="hint">You need Admin access to manage users.</p>
      </ng-template>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 20px 0; font-size: 16px; font-weight: 600; }
    .hint { color: #888; font-size: 12px; margin-top: 8px; }
    .btn-cell { display: flex; align-items: flex-end; }
    .btn-cell .btn-primary { white-space: nowrap; }
  `]
})
export class UsersComponent implements OnInit {
  users: UserAccount[] = [];
  newId = '';
  newPassword = '';
  newAccess = 'Viewer';
  accessLevels = ['Admin', 'Viewer'];
  // Tenant options: default + platform-operator (*) + every active tenant.
  newTenant = 'default';
  tenantIds: string[] = ['default', '*'];
  loading = false;
  saving = false;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private session: SessionService,
              private toast: ToastService) {}

  ngOnInit(): void { if (this.isAdmin) { this.reload(); this.loadTenants(); } }

  reload(): void {
    this.loading = true;
    this.http.listUsers().subscribe({
      next: (r) => { this.loading = false; if (r.ok && r.users) this.users = r.users; },
      error: () => { this.loading = false; }
    });
  }

  /// Populate the Tenant dropdown: default + "*" (platform operator) + the
  /// active tenants from cloud.tenants.
  loadTenants(): void {
    this.http.dbGet(['cloud.tenants']).subscribe({
      next: (r) => {
        const ids: string[] = ['default', '*'];
        if (r.ok && r.data) {
          try {
            const arr = JSON.parse(String(r.data['cloud.tenants'] ?? '[]'));
            (Array.isArray(arr) ? arr : [])
              .filter((t) => (t?.status || 'active') === 'active' && t?.id && t.id !== 'default')
              .forEach((t) => ids.push(String(t.id)));
          } catch { /* keep defaults */ }
        }
        this.tenantIds = ids;
      },
      error: () => {}
    });
  }

  create(): void {
    if (!this.newId || !this.newPassword) { this.toast.error('ID and password required'); return; }
    this.saving = true;
    this.http.createUser({ id: this.newId, password: this.newPassword,
                           access: this.newAccess, tenant: this.newTenant }).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) {
          this.toast.success('User ' + r.id + ' saved');
          this.newId = ''; this.newPassword = ''; this.newAccess = 'Viewer'; this.newTenant = 'default';
          this.reload();
        } else { this.toast.error(r.err || 'Failed to save user'); }
      },
      error: (e) => { this.saving = false; this.toast.error(e?.error?.err || 'Failed to save user'); }
    });
  }

  remove(id: string): void {
    this.http.deleteUser(id).subscribe({
      next: (r) => {
        if (r.ok) { this.toast.success('User ' + id + ' deleted'); this.reload(); }
        else { this.toast.error(r.err || 'Delete failed'); }
      },
      error: (e) => this.toast.error(e?.error?.err || 'Delete failed')
    });
  }
}
