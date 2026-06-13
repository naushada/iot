import { Component, OnInit } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';

@Component({
  selector: 'app-bs-config',
  templateUrl: './bs-config.component.html',
  styles: [`
    .page { padding: 24px; }
    h3 { color: #333; margin: 0 0 8px 0; font-size: 16px; font-weight: 600; }
    h4 { color: #555; margin: 0 0 16px 0; font-size: 14px; font-weight: 600;
         border-bottom: 1px solid #e0e0e0; padding-bottom: 8px; }
    .hint { color: #888; font-weight: normal; font-size: 11px; }
  `]
})
export class BsConfigComponent implements OnInit {
  bsForm: FormGroup;
  loading = true;
  savingBs = false;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(
    private http: HttpsvcService,
    fb: FormBuilder,
    private session: SessionService,
    private toast: ToastService
  ) {
    // Only the keys the bootstrap server actually consumes at /bs:
    // cloud.bs.uri (Security/0 RID0) and cloud.dm.uri (Security/1 RID0).
    // Per-device PSK identity/key are minted per-endpoint into
    // cloud.endpoint.credentials — see the Endpoints page.
    this.bsForm = fb.group({
      bs_uri:        ['coaps://0.0.0.0:5684'],
      dm_uri:        ['coaps://0.0.0.0:5683'],
    });
  }

  ngOnInit(): void {
    this.http.dbGet([
      'cloud.bs.uri', 'cloud.dm.uri'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.bsForm.patchValue({
            bs_uri:        d['cloud.bs.uri']           || 'coaps://0.0.0.0:5684',
            dm_uri:        d['cloud.dm.uri']           || 'coaps://0.0.0.0:5683',
          });
        }
        this.loading = false;
      },
      error: () => { this.loading = false; }
    });
  }

  saveBs(): void {
    this.savingBs = true;
    const v = this.bsForm.value;
    this.http.dbSet([
      { key: 'cloud.bs.uri',           value: v.bs_uri },
      { key: 'cloud.dm.uri',           value: v.dm_uri },
    ]).subscribe({
      next: (r) => {
        this.savingBs = false;
        if (r.ok) this.toast.success('BS config saved');
        else this.toast.error(r.err || 'Save failed');
      },
      error: () => { this.savingBs = false; this.toast.error('Save failed'); }
    });
  }
}
