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
    this.bsForm = fb.group({
      endpoint:      ['urn:dev:gateway-'],
      bs_uri:        ['coaps://0.0.0.0:5684'],
      security_mode: ['PSK'],
      psk_id:        ['iot-client'],
      psk_key:       [''],
      dm_uri:        ['coaps://0.0.0.0:5683'],
    });
  }

  ngOnInit(): void {
    this.http.dbGet([
      'cloud.bs.endpoint', 'cloud.bs.uri', 'cloud.bs.security.mode',
      'cloud.bs.psk.id', 'cloud.bs.psk.key', 'cloud.dm.uri'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.bsForm.patchValue({
            endpoint:      d['cloud.bs.endpoint']      || 'urn:dev:gateway-',
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

  saveBs(): void {
    this.savingBs = true;
    const v = this.bsForm.value;
    this.http.dbSet([
      { key: 'cloud.bs.endpoint',      value: v.endpoint },
      { key: 'cloud.bs.uri',           value: v.bs_uri },
      { key: 'cloud.bs.security.mode', value: v.security_mode },
      { key: 'cloud.bs.psk.id',        value: v.psk_id },
      { key: 'cloud.bs.psk.key',       value: v.psk_key },
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
