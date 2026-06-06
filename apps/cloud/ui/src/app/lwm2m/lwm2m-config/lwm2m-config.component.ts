import { Component, OnInit } from '@angular/core';
import { FormBuilder, FormGroup } from '@angular/forms';
import { HttpsvcService } from '../../../common/httpsvc.service';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';

@Component({
  selector: 'app-lwm2m-config',
  templateUrl: './lwm2m-config.component.html',
  styleUrls: ['./lwm2m-config.component.scss']
})
export class Lwm2mConfigComponent implements OnInit {
  dmForm: FormGroup;
  loading = true; saving = false;

  get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(
    private http: HttpsvcService,
    fb: FormBuilder,
    private session: SessionService,
    private toast: ToastService
  ) {
    this.dmForm = fb.group({
      dm_uri:        ['coaps://0.0.0.0:5683'],
      lifetime:      [86400],
      binding:       ['U'],
      dm_psk_id:     ['iot-dm-client'],
      dm_psk_key:    [''],
      lwm2m_version: ['1.1'],
    });
  }

  ngOnInit(): void {
    this.http.dbGet([
      'cloud.dm.uri', 'cloud.dm.lifetime', 'cloud.dm.binding',
      'cloud.dm.psk.id', 'cloud.dm.psk.key', 'cloud.dm.lwm2m.version'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.dmForm.patchValue({
            dm_uri:        d['cloud.dm.uri']             || 'coaps://0.0.0.0:5683',
            lifetime:      d['cloud.dm.lifetime']        || 86400,
            binding:       d['cloud.dm.binding']         || 'U',
            dm_psk_id:     d['cloud.dm.psk.id']          || 'iot-dm-client',
            dm_psk_key:    d['cloud.dm.psk.key']         || '',
            lwm2m_version: d['cloud.dm.lwm2m.version']   || '1.1',
          });
        }
        this.loading = false;
      },
      error: () => { this.loading = false; }
    });
  }

  save(): void {
    this.saving = true;
    const v = this.dmForm.value;
    this.http.dbSet([
      { key: 'cloud.dm.uri',              value: v.dm_uri },
      { key: 'cloud.dm.lifetime',         value: v.lifetime },
      { key: 'cloud.dm.binding',          value: v.binding },
      { key: 'cloud.dm.psk.id',           value: v.dm_psk_id },
      { key: 'cloud.dm.psk.key',          value: v.dm_psk_key },
      { key: 'cloud.dm.lwm2m.version',    value: v.lwm2m_version },
    ]).subscribe({
      next: (r) => {
        this.saving = false;
        if (r.ok) this.toast.success('DM config saved');
        else this.toast.error(r.err || 'Save failed');
      },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
