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
    // Only the keys the DM server actually consumes when building the
    // Security/Server TLVs at /bs: cloud.dm.uri, cloud.dm.lifetime,
    // cloud.dm.binding. Per-device DM PSK identity/key are minted
    // per-endpoint into cloud.endpoint.credentials — see the Endpoints page.
    this.dmForm = fb.group({
      dm_uri:        ['coaps://0.0.0.0:5683'],
      lifetime:      [86400],
      binding:       ['U'],
    });
  }

  ngOnInit(): void {
    this.http.dbGet([
      'cloud.dm.uri', 'cloud.dm.lifetime', 'cloud.dm.binding'
    ]).subscribe({
      next: (r) => {
        if (r.ok && r.data) {
          const d = r.data as Record<string, unknown>;
          this.dmForm.patchValue({
            dm_uri:        d['cloud.dm.uri']             || 'coaps://0.0.0.0:5683',
            lifetime:      d['cloud.dm.lifetime']        || 86400,
            binding:       d['cloud.dm.binding']         || 'U',
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
