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
    h3 { color: #333; margin: 0 0 20px 0; font-size: 16px; font-weight: 600; }
    .hint { color: #888; font-weight: normal; font-size: 11px; }
    .info-card {
      background: #f0f5ff; border: 1px solid #b3d4ff; border-radius: 4px;
      padding: 12px 16px; font-size: 13px; color: #333;
    }
    .info-card code { background: #d0e4ff; padding: 1px 4px; border-radius: 2px; font-size: 12px; }
  `]
})
export class BsConfigComponent implements OnInit {
  bsForm: FormGroup;
  loading = true; saving = false;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(
    private http: HttpsvcService,
    fb: FormBuilder,
    private session: SessionService,
    private toast: ToastService
  ) {
    this.bsForm = fb.group({
      bs_uri:        ['coaps://0.0.0.0:5684'],
      security_mode: ['PSK'],
      psk_id:        ['iot-client'],
      psk_key:       [''],
      dm_uri:        ['coaps://0.0.0.0:5683'],
    });
  }

  ngOnInit(): void {
    this.http.dbGet([
      'cloud.bs.uri', 'cloud.bs.security.mode',
      'cloud.bs.psk.id', 'cloud.bs.psk.key',
      'cloud.dm.uri'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.bsForm.patchValue({
            bs_uri:        d['cloud.bs.uri']           || 'coaps://0.0.0.0:5684',
            security_mode: d['cloud.bs.security.mode'] || 'PSK',
            psk_id:        d['cloud.bs.psk.id']        || 'iot-client',
            psk_key:       d['cloud.bs.psk.key']       || '',
            dm_uri:        d['cloud.dm.uri']           || 'coaps://0.0.0.0:5683',
          });
        }
        this.loading = false;
      },
      error: () => { this.loading = false; }
    });
  }

  save(): void {
    this.saving = true;
    const v = this.bsForm.value;
    this.http.dbSet([
      { key: 'cloud.bs.uri',           value: v.bs_uri },
      { key: 'cloud.bs.security.mode', value: v.security_mode },
      { key: 'cloud.bs.psk.id',        value: v.psk_id },
      { key: 'cloud.bs.psk.key',       value: v.psk_key },
      { key: 'cloud.dm.uri',           value: v.dm_uri },
    ]).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) this.toast.success('BS config saved');
        else this.toast.error(r.err || 'Save failed');
      },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
