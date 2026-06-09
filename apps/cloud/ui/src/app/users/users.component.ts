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
          </clr-input-container>
          <clr-password-container>
            <label>Password</label>
            <input clrPassword type="password" [(ngModel)]="newPassword" placeholder="••••••••" />
          </clr-password-container>
          <clr-select-container>
            <label>Access</label>
            <select clrSelect [(ngModel)]="newAccess">
              <option *ngFor="let a of accessLevels" [value]="a">{{ a }}</option>
            </select>
          </clr-select-container>
          <div class="btn-cell">
            <button class="btn btn-primary" (click)="create()" [disabled]="saving">
              {{ saving ? 'Saving…' : 'Add / Update' }}
            </button>
          </div>
        </div>
        <p class="hint">Existing ID updates that user's password &amp; access. The built-in <code>admin</code> can't be created or deleted here.</p>

        <!-- User list -->
        <table class="table table-compact" style="margin-top:20px;">
          <thead>
            <tr><th class="left">User ID</th><th class="left">Access</th><th></th></tr>
          </thead>
          <tbody>
            <tr *ngIf="loading"><td colspan="3">Loading…</td></tr>
            <tr *ngFor="let u of users">
              <td class="left">{{ u.id }}</td>
              <td class="left">
                <app-status-badge [label]="u.access" [state]="u.access==='Admin' ? 'connected' : 'idle'"></app-status-badge>
              </td>
              <td class="right">
                <button class="btn btn-sm btn-danger-outline" *ngIf="u.id !== 'admin'" (click)="remove(u.id)">Delete</button>
                <span *ngIf="u.id === 'admin'" class="hint">built-in</span>
              </td>
            </tr>
          </tbody>
        </table>
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
    table { width: 100%; }
    th.left, td.left { text-align: left; }
    td.right, th:last-child { text-align: right; }
  `]
})
export class UsersComponent implements OnInit {
  users: UserAccount[] = [];
  newId = '';
  newPassword = '';
  newAccess = 'Viewer';
  accessLevels = ['Admin', 'Viewer'];
  loading = false;
  saving = false;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(private http: HttpsvcService, private session: SessionService,
              private toast: ToastService) {}

  ngOnInit(): void { if (this.isAdmin) this.reload(); }

  reload(): void {
    this.loading = true;
    this.http.listUsers().subscribe({
      next: (r) => { this.loading = false; if (r.ok && r.users) this.users = r.users; },
      error: () => { this.loading = false; }
    });
  }

  create(): void {
    if (!this.newId || !this.newPassword) { this.toast.error('ID and password required'); return; }
    this.saving = true;
    this.http.createUser({ id: this.newId, password: this.newPassword, access: this.newAccess }).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) {
          this.toast.success('User ' + r.id + ' saved');
          this.newId = ''; this.newPassword = ''; this.newAccess = 'Viewer';
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
