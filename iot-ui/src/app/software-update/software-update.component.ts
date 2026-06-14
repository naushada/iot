import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { HttpsvcService } from '../../common/httpsvc.service';
import { DataStoreService } from '../../common/datastore.service';
import { SessionService } from '../../common/session.service';
import { ToastService } from '../../common/toast.service';

@Component({
  selector: 'app-software-update',
  template: `
    <div class="page">
      <h3>Software Update</h3>

      <ng-container *ngIf="isAdmin; else noAccess">
        <!-- Current state -->
        <clr-datagrid style="margin-bottom:20px;">
          <clr-dg-column>Property</clr-dg-column>
          <clr-dg-column>Value</clr-dg-column>
          <clr-dg-row>
            <clr-dg-cell>Installed Version
              <app-ds-hint *dsDebug key="iot.update.version"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell>{{ version || '—' }}</clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row>
            <clr-dg-cell>State
              <app-ds-hint *dsDebug key="iot.update.state"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell><app-status-badge [label]="stateLabel"
              [state]="state===0 ? 'connected' : 'idle'"></app-status-badge></clr-dg-cell>
          </clr-dg-row>
          <clr-dg-row>
            <clr-dg-cell>Result
              <app-ds-hint *dsDebug key="iot.update.result"></app-ds-hint></clr-dg-cell>
            <clr-dg-cell><app-status-badge [label]="resultLabel"
              [state]="result===1 ? 'connected' : (result>=5 ? 'exited' : 'idle')"></app-status-badge></clr-dg-cell>
          </clr-dg-row>
        </clr-datagrid>

        <!-- Self-update trigger -->
        <h4>Apply a package (.ipk URL)</h4>
        <div class="form-grid" style="align-items:end;">
          <clr-input-container style="grid-column: span 3;">
            <label>Package URL</label>
            <input clrInput [(ngModel)]="url" style="width:100%;"
                   placeholder="https://<cloud-host>/firmware/<package>.ipk?sha256=<hash>" />
            <clr-control-helper *dsDebug><app-ds-hint key="iot.update.request"></app-ds-hint></clr-control-helper>
          </clr-input-container>
          <div class="btn-cell">
            <button class="btn btn-primary" (click)="apply()" [disabled]="busy || !url">
              {{ busy ? 'Applying…' : 'Apply' }}
            </button>
          </div>
        </div>
        <p class="hint">Runs <code>opkg install</code> on the device and restarts the affected daemons. The URL may carry <code>?sha256=</code> &amp; <code>?version=</code>.</p>

        <!-- Drag-and-drop local package (chunked upload) -->
        <h4>Or drop a package</h4>
        <div class="dropzone" [class.over]="dragOver" [class.busy]="uploading"
             (dragover)="onDragOver($event)" (dragleave)="onDragLeave($event)"
             (drop)="onDrop($event)" (click)="fileInput.click()">
          <input #fileInput type="file" accept=".ipk,.tar.gz,.tgz,.raucb" hidden
                 (change)="onPick($event)" />
          <clr-icon shape="upload-cloud" size="28"></clr-icon>
          <span *ngIf="!uploading">Drag &amp; drop a <code>.ipk</code> / <code>.tar.gz</code> / <code>.raucb</code>, or click to browse</span>
          <span *ngIf="uploading">Uploading {{ uploadName }} — {{ uploadPct }}%…</span>
        </div>
        <p class="hint">Uploaded to the device in chunks and installed there:
          <code>.ipk</code>/<code>.tar.gz</code> via <code>opkg</code>,
          <code>.raucb</code> via <code>rauc</code> (A/B image — requires the RAUC-enabled image).</p>
      </ng-container>

      <ng-template #noAccess><p class="hint">You need Admin access to update software.</p></ng-template>
    </div>
  `,
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 16px 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 18px 0 10px 0; font-size: 13px; font-weight: 600; }
    .hint { color: #888; font-size: 12px; margin-top: 8px; }
    .btn-cell { display: flex; align-items: flex-end; }
    .btn-cell .btn-primary { white-space: nowrap; }
    .dropzone { display: flex; align-items: center; gap: 12px; padding: 18px;
      border: 2px dashed #b0bec5; border-radius: 8px; color: #607d8b;
      cursor: pointer; font-size: 13px; transition: all 0.15s; background: #fafcfd; }
    .dropzone:hover, .dropzone.over { border-color: #0072a3; color: #0072a3; background: #f0f8fc; }
    .dropzone.busy { opacity: 0.6; pointer-events: none; }
  `]
})
export class SoftwareUpdateComponent implements OnInit, OnDestroy {
  version = '';
  state = 0;
  result = 0;
  url = '';
  busy = false;
  dragOver = false;
  uploading = false;
  uploadName = '';
  uploadPct = 0;
  private sub = new Subscription();

  get isAdmin(): boolean { return this.session.isAdmin; }
  get stateLabel(): string {
    return ['idle', 'downloading', 'downloaded', 'updating'][this.state] || 'unknown';
  }
  get resultLabel(): string {
    if (this.result === 0) return '—';
    if (this.result === 1) return 'success';
    if (this.result === 5) return 'integrity error';
    if (this.result === 8) return 'uri error';
    if (this.result === 9) return 'install error';
    return 'error ' + this.result;
  }

  constructor(private http: HttpsvcService, private ds: DataStoreService,
              private session: SessionService, private toast: ToastService) {}

  ngOnInit(): void {
    if (!this.isAdmin) return;
    // Read OTA progress live off the single shared /status stream — no
    // per-page 5s self-poll. The long-poll wakes on iot.update.state changes,
    // so download/install progress renders promptly without missing updates.
    this.sub.add(this.ds.observe('iot.update.version').subscribe(v => {
      if (v != null) this.version = String(v);
    }));
    this.sub.add(this.ds.observe('iot.update.state').subscribe(v => {
      this.state = Number(v) || 0;
    }));
    this.sub.add(this.ds.observe('iot.update.result').subscribe(v => {
      this.result = Number(v) || 0;
    }));
  }

  apply(): void {
    if (!this.url) { this.toast.error('Package URL required'); return; }
    this.busy = true;
    this.http.dbSet([{ key: 'iot.update.request', value: this.url }]).subscribe({
      next: (r) => {
        this.busy = false;
        if (r.ok) { this.toast.success('Update started'); }
        else { this.toast.error(r.err || 'Update failed'); }
      },
      error: () => { this.busy = false; this.toast.error('Update failed'); }
    });
  }

  // ── Drag-and-drop upload ──────────────────────────────────────────
  onDragOver(e: DragEvent): void { e.preventDefault(); this.dragOver = true; }
  onDragLeave(e: DragEvent): void { e.preventDefault(); this.dragOver = false; }
  onDrop(e: DragEvent): void {
    e.preventDefault();
    this.dragOver = false;
    const f = e.dataTransfer?.files?.[0];
    if (f) this.uploadFile(f);
  }
  onPick(e: Event): void {
    const input = e.target as HTMLInputElement;
    const f = input.files?.[0];
    if (f) this.uploadFile(f);
    input.value = '';   // allow re-picking the same file
  }

  // ≤8 MiB chunks (under the server body cap); posted sequentially so an
  // arbitrarily large .raucb bundle uploads without buffering it server-side.
  private static readonly CHUNK = 4 * 1024 * 1024;

  private uploadFile(file: File): void {
    if (this.uploading) return;
    if (!/\.(ipk|tar\.gz|tgz|raucb)$/i.test(file.name)) {
      this.toast.error('Pick a .ipk, .tar.gz, .tgz or .raucb file'); return;
    }
    this.uploading = true; this.uploadName = file.name; this.uploadPct = 0;
    const total = file.size;
    const sendFrom = (offset: number): void => {
      const end = Math.min(offset + SoftwareUpdateComponent.CHUNK, total);
      const final = end >= total;
      this.sub.add(this.http.uploadUpdateChunk(file.name, file.slice(offset, end), offset, final).subscribe({
        next: (r) => {
          if (!r.ok) { this.uploading = false; this.toast.error(r.err || 'Upload failed'); return; }
          this.uploadPct = total ? Math.round((end / total) * 100) : 100;
          if (final) { this.uploading = false; this.toast.success('Uploaded — installing ' + file.name + '…'); }
          else sendFrom(end);
        },
        error: (e) => { this.uploading = false; this.toast.error((e?.error?.err) || 'Upload failed'); }
      }));
    };
    sendFrom(0);
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }
}
