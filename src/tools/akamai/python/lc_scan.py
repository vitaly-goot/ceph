'''
JIRA: https://track.akamai.com/jira/browse/OBJGEN1-1235

Developer: @vgoot
Code Review: @sung @kkeshava

This script scans bucket lifecycle policies and provides insights into the count and size metrics of expired versus non-expired objects for current, non-current, and incomplete multipart uploads.

It downloads and parses the S3 bucket lifecycle policy (as defined here https://docs.aws.amazon.com/AmazonS3/latest/userguide/intro-lifecycle-rules.html), then iterates over the bucket to verify
if an object is expired. The script reports counters that outline the number of objects and their size. It also calculates and reports the ratio of expired to non-expired objects.
During iteration, the script produces a report line labeled 'HEARTBEAT' with intermediate results, followed by the last processed token. The counters are reported as:
  Expired: [{expired_obj}, {expired_byte}] Total: [{total_obj}, {total_byte}] Rate: [{rate_obj}%, {rate_byte}%]

The script supports aborting and resuming processing from a specific token at any time, resuming the calculation with restored counters for current and non-current objects. For example:
  python3 lc_scan.py --bucket=test4 --token vgoot%2F7%2F172.232.160.215%2F76 --current_cnt 1,100,5,500 --noncurrent_cnt 0,0,10,1000

Here, the counters are provided as a tuple of four integers separated by commas, denoting (expired_obj, expired_byte, total_obj, total_byte) for corresponded counter.

At the end, the script iterates over the list of incomplete uploads and produces a counter showing expired versus non-expired uploads according to the Lifecycle policy rule, if one is present.
You can skip the verification of current and non-current objects by using the --mpu_only command option flag.

Notes:
- Tags policies are not supported, mainly to improve speed of scanning as we don't use it at Akamai. Contact developers if you would like to add support for it.
- Ceph-17 does not support the NewerNoncurrentVersions attribute in S3 lifecycle policies. If you attempt to set a rule with this attribute, Ceph-17 will silently exclude it from the policy rules, resulting in a final rule that does not include the NewerNoncurrentVersions attribute.
- Ceph-17 does not support complex rules with object size filters defined in the And section. For example, attempting to set the following rule will result in the ObjectSizeGreaterThan and ObjectSizeLessThan attributes being dropped:
       'And': {
            'Prefix': 'logs/',
            'Tags': [{'Key': 'Environment', 'Value': 'Production'}],
            'ObjectSizeGreaterThan': 1000,
            'ObjectSizeLessThan': 100000
        }
'''
import sys
import traceback
import time
import boto3
import unittest
import logging
import http.client as http_client
import argparse
import json
import numpy as np
from datetime import datetime, timedelta, timezone
from dateutil.tz import tzutc
from botocore.client import Config
from collections import Counter
from unittest.mock import patch, MagicMock

'''
Avoid exposing credentials on the command line.
You can change it directly below in the code or alternatively use --config option to overwrite it with json configuration file.
Example JSON File:
{
  "args": {
    "access_key": "YOUR_NEW_ACCESS_KEY",
    "secret_key": "YOUR_NEW_SECRET_KEY",
    "endpoint_url": "http://new-endpoint-url.com"
  }
}
'''
args = {
    "access_key": "VUNZIJE94C031609RLAU",
    "secret_key": "5rZLsv1Hmw43aBsXxIWIlRvxVj9phtIP7gCWaqVz",
    "endpoint_url": "http://us-sea-3.linodeobjects.com",
}
retries = {
    'mode': 'standard',       # or 'adaptive'
    'max_attempts': 1
}
s3=None

class Utils:
    @staticmethod
    def round_to_next_midnight_utc(date_object: datetime) -> datetime:
        # Ensure the date_object is in UTC
        if date_object.tzinfo is None:
            date_object = date_object.replace(tzinfo=timezone.utc)
        else:
            date_object = date_object.astimezone(timezone.utc)

        # Add one day to the date part
        next_day = date_object.date() + timedelta(days=1)

        # Combine the next day date with midnight time
        next_midnight = datetime.combine(next_day, datetime.min.time()).replace(tzinfo=timezone.utc)

        return next_midnight


class TupleAction(argparse.Action):
    def __init__(self, option_strings, dest, nargs=None, **kwargs):
        if 'default' not in kwargs:
            kwargs['default'] = (0,0,0,0)
        super(TupleAction, self).__init__(option_strings, dest, nargs=nargs, **kwargs)

    def __call__(self, parser, namespace, values, option_string=None):
        # Split the input string by comma and convert to a tuple of integers
        try:
            var = tuple(map(int, values.split(',')))
            if len(var) != 4:
                raise ValueError
        except ValueError:
            raise argparse.ArgumentTypeError("Argument must be four integers separated by a comma")

        # Set the value in the namespace
        setattr(namespace, self.dest, var)

    def __get_default__(self):
        return (0,0,0,0)


class LifecycleCounter(Counter):
    def __init__(self, name: str, cnt: tuple):
        self.name = name
        super().__init__({
            'expired_obj': cnt[0],
            'expired_byte': cnt[1],
            'total_obj': cnt[2],
            'total_byte': cnt[3],
        })

    def count_total(self, byte: int):
        self['total_obj'] += 1
        self['total_byte'] += byte

    def count_expired(self, byte: int):
        self['expired_obj'] += 1
        self['expired_byte'] += byte

    def __str__(self) -> str:
        expired_obj = self['expired_obj']
        expired_byte = self['expired_byte']
        total_obj = self['total_obj']
        total_byte = self['total_byte']
        
        rate_obj = (expired_obj / total_obj * 100) if total_obj > 0 else 0.0
        rate_byte = (expired_byte / total_byte * 100) if total_byte > 0 else 0.0
        
        return (f"{self.name}:"
                f"{{Expired:[{expired_obj},{expired_byte}]}}"
                f",{{Total:[{total_obj},{total_byte}]}}"
                f",{{Rate:[{rate_obj:.1f}%,{rate_byte:.1f}%]}}")

class LCContext:
    def __init__(self, current_counter: LifecycleCounter, noncurrent_counter: LifecycleCounter, mpu_counter: LifecycleCounter):
        self.current_counter = current_counter
        self.noncurrent_counter = noncurrent_counter
        self.mpu_counter = mpu_counter
        self.testmode = False

class LifecycleRule:
    def __init__(self, ctx: LCContext, rule: json):
        self.ctx = ctx
        self.rule_id = rule.get('ID')
        self.status = rule.get('Status')
        self.prefix = None
        self.tags = []
        self.object_size_greater_than = None
        self.object_size_less_than = None
        self.expiration_date = None
        self.transition_days = None
        self.storage_class = None
        self.noncurrent_date = None
        self.newer_noncurrent_versions = None
        self.incomplete_mpu_date = None

        filter_element = rule.get('Filter', {})
        if 'And' in filter_element:
            self._parse_and_filter(filter_element['And'])
        else:
            self._parse_simple_filter(filter_element)

        expiration = rule.get('Expiration', {})
        expiration_days = expiration.get('Days')
        expiration_date = expiration.get('Date')
        if expiration_days:
          self.expiration_date = datetime.utcnow() - timedelta(days=expiration_days)
        elif expiration_date:  
          self.expiration_date = expiration_date

        if self.expiration_date:
            self.expiration_date = Utils.round_to_next_midnight_utc(self.expiration_date)

        transition = rule.get('Transition', {})
        self.transition_days = transition.get('Days')
        self.storage_class = transition.get('StorageClass')
        #raise ValueError("Transition not supported.")
        
        noncurrent_expiration = rule.get('NoncurrentVersionExpiration', {})
        noncurrent_days = noncurrent_expiration.get('NoncurrentDays')
        if noncurrent_days:
          self.noncurrent_date = Utils.round_to_next_midnight_utc(
              datetime.utcnow() - timedelta(days=noncurrent_days))
        self.newer_noncurrent_versions = noncurrent_expiration.get('NewerNoncurrentVersions')

        abort_incomplete_mpu = rule.get('AbortIncompleteMultipartUpload', {}) 
        incomplete_mpu_days = abort_incomplete_mpu.get('DaysAfterInitiation')
        if incomplete_mpu_days:
          self.incomplete_mpu_date = Utils.round_to_next_midnight_utc(
              datetime.utcnow() - timedelta(days=incomplete_mpu_days))

    def active(self) -> [bool,bool,bool]:
        if self.status != "Enabled":
            return [False, False, False]
        return [self.expiration_date is not None, self.noncurrent_date is not None or self.newer_noncurrent_versions is not None, self.incomplete_mpu_date is not None]

    def _parse_and_filter(self, and_filter: json):
        self.prefix = and_filter.get('Prefix')

        for tag in and_filter.get('Tags', []):
            self.tags.append((tag['Key'], tag['Value']))
            if not self.ctx.testmode: 
                raise ValueError("Tags policies are not supported, mainly to improve speed of scanning as we don't use it at Akamai. Contact developers if you would like to add support for it.")

        self.object_size_greater_than = and_filter.get('ObjectSizeGreaterThan')
        self.object_size_less_than = and_filter.get('ObjectSizeLessThan')

    def _parse_simple_filter(self, filter_element: json):
        prefix = filter_element.get('Prefix', None)
        tag = filter_element.get('Tag', None)
        object_size_greater_than = filter_element.get('ObjectSizeGreaterThan', None)
        object_size_less_than = filter_element.get('ObjectSizeLessThan', None)

        # Count the number of active elements
        active_elements = sum([
            prefix is not None,
            tag is not None,
            object_size_greater_than is not None,
            object_size_less_than is not None
        ])

        if active_elements > 1:
            raise ValueError("Filter cannot contain more than one of Prefix, Tag, ObjectSizeGreaterThan, or ObjectSizeLessThan elements without AND section.")

        if tag is not None and not self.ctx.testmode: 
            raise ValueError("Tags policies are not supported, mainly to improve speed of scanning as we don't use it at Akamai. Contact developers if you would like to add support for it.")
         
        self.prefix = prefix
        self.object_size_greater_than = object_size_greater_than
        self.object_size_less_than = object_size_less_than

    def expire_current(self, obj: json) -> bool:
        if self.status != "Enabled":
            return False
        if self.expiration_date is None: 
            return False
        if self.prefix and not obj['Key'].startswith(self.prefix):
            return False
        if self.object_size_less_than and obj['Size'] >= self.object_size_less_than:
            return False
        if self.object_size_greater_than and obj['Size'] <= self.object_size_greater_than:
            return False
        if (obj['LastModified'].replace(tzinfo=timezone.utc) > self.expiration_date):
            return False
        self.ctx.current_counter.count_expired(obj['Size'])
        return True 

    def expire_noncurrent(self, versions: json) -> bool:
        if self.status != "Enabled":
            return False
        if self.noncurrent_date is None and self.newer_noncurrent_versions is None:
            return False
        if self.prefix and not versions[0]['Key'].startswith(self.prefix):
            return False

        rev = 0
        res = False
        for version in versions:
            rev += 1
            if version['IsLatest']: continue
            if self.newer_noncurrent_versions and rev > self.newer_noncurrent_versions:
                self.ctx.noncurrent_counter.count_expired(version['Size'])
                res = True
            elif self.noncurrent_date and version['LastModified'].replace(tzinfo=timezone.utc) < self.noncurrent_date:
                self.ctx.noncurrent_counter.count_expired(version['Size'])
                res = True
        return res

    def expire_mpu(self, obj, total_size: json) -> bool:
        if self.status != "Enabled":
            return False
        if self.incomplete_mpu_date is None: 
            return False
        if self.prefix and not obj['Key'].startswith(self.prefix):
            return False
        if self.object_size_less_than and total_size >= self.object_size_less_than:
            return False
        if self.object_size_greater_than and total_size <= self.object_size_greater_than:
            return False
        if (obj['Initiated'].replace(tzinfo=timezone.utc) > self.incomplete_mpu_date):
            return False
        self.ctx.mpu_counter.count_expired(total_size)
        return True     

    def display(self):
        print("---")
        print(f"Rule ID: {self.rule_id}")
        print(f"Status: {self.status}")
        if self.prefix:
            print(f"Prefix: {self.prefix}")
        for key, value in self.tags:
            print(f"Tag: {key} = {value}")
        if self.object_size_greater_than:
            print(f"Object Size Greater Than: {self.object_size_greater_than}")
        if self.object_size_less_than:
            print(f"Object Size Less Than: {self.object_size_less_than}")
        if self.expiration_date is not None:
            print(f"Expiration Date: {self.expiration_date}")
        if self.transition_days is not None:
            print(f"Transition Days: {self.transition_days}, Storage Class: {self.storage_class}")
        if self.noncurrent_date is not None:
            print(f"Noncurrent Version Expiration Days: {self.noncurrent_date}")
        if self.newer_noncurrent_versions is not None:
            print(f"Noncurrent Newer Version: {self.newer_noncurrent_versions}")
        if self.incomplete_mpu_date is not None:
            print(f"Abort Incomplete Multipart Upload Date: {self.incomplete_mpu_date}")

    def __str__(self) -> str:
        return (f"{self.rule_id}:{self.status},"
                f"Prefix:{self.prefix}," 
                f"Size:[{self.object_size_greater_than},{self.object_size_less_than}]," 
                f"Expiration:[{self.expiration_date},{self.noncurrent_date},{self.incomplete_mpu_date}],"
                f"Noncurrent_Versions:{self.newer_noncurrent_versions}")

def lc_obj_processor(ctx: LCContext, current: bool, non_current: bool, rules: json):
    kwargs = {
        "Bucket": cmd_args.bucket,
        "MaxKeys": 1000,  
        "ContinuationToken": cmd_args.token
    }
    try:
        while True:
            response = s3.meta.client.list_objects_v2(**kwargs)
            ntoken = response.get("NextContinuationToken")
            if 'Contents' not in response: break

            for obj in response['Contents']:
                #print(obj['LastModified'])
                ctx.current_counter.count_total(obj['Size'])
                if current: 
                    for rule in rules:
                        #print(rule.__dict__)
                        if rule.expire_current(obj):
                            break

                if non_current:
                    response = s3.meta.client.list_object_versions(Bucket=cmd_args.bucket, Prefix=obj['Key'])    
                    versions = response.get('Versions', [])
                    if versions:
                        for version in versions:
                            if not version['IsLatest']:
                                ctx.noncurrent_counter.count_total(version['Size'])
                        for rule in rules:
                            #print(rule.__dict__)
                            if rule.expire_noncurrent(versions):
                                break

                if ctx.current_counter['total_obj'] % cmd_args.heartbeat == 0:
                    print('%d HEARTBEAT %s %s' % (time.time(), ctx.current_counter, ctx.noncurrent_counter))
                    if ntoken: print(ntoken)
                    sys.stdout.flush()

            if not ntoken: break
            kwargs["ContinuationToken"] = ntoken

    except:    
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)

def lc_mpu_processor(ctx: LCContext, rules: json):
    try:
        paginator = s3.meta.client.get_paginator('list_multipart_uploads')
        page_iterator = paginator.paginate(Bucket=cmd_args.bucket)

        for page in page_iterator:
            uploads = page.get('Uploads', [])
            if not uploads:
                print("No in-progress multipart uploads found.")
                continue
            
            for upload in uploads:
                part_paginator = s3.meta.client.get_paginator('list_parts')
                part_iterator = part_paginator.paginate(Bucket=cmd_args.bucket, Key=upload['Key'], UploadId=upload['UploadId'])
                total_size = 0
                for part_page in part_iterator:
                    parts = part_page.get('Parts', [])
                    for part in parts:
                        total_size += part['Size']
                ctx.mpu_counter.count_total(total_size)
                for rule in rules:
                    #print(rule.__dict__)
                    if rule.expire_mpu(upload, total_size):
                        break
    
    except:    
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)

def main():
    global args
    global retries
    global s3
    global cmd_args

    if cmd_args.config:
         with open(cmd_args.config, 'r') as file:
            override = json.load(file)
            if 'args' in override: 
                args.update(override['args'])
            if 'retries' in override: 
                retries.update(override['retries'])

    ctx = LCContext(
        LifecycleCounter('Current', cmd_args.current_cnt),
        LifecycleCounter('Noncurrent', cmd_args.noncurrent_cnt),
        LifecycleCounter('MPU', (0,0,0,0)))

    session = boto3.Session(aws_access_key_id=args['access_key'], aws_secret_access_key=args['secret_key'])
    s3 = session.resource('s3', endpoint_url=args['endpoint_url'], config=Config(retries=retries))
    response = s3.meta.client.get_bucket_lifecycle_configuration(Bucket=cmd_args.bucket)

    rules = []
    for rule_data in response['Rules']:
        rule = LifecycleRule(ctx, rule_data)
        rules.append(rule)
        rule.display()

    combined_rules = np.array([[False, False, False]])
    for rule in rules:
        combined_rules = np.vstack((combined_rules, [rule.active()]))

    [current, non_current, mpu] = np.any(combined_rules, axis=0)
    print('===')
    print(f"Combined rules execution plan: Current:{current},Noncurrent:{non_current},MPU:{mpu}")
    if cmd_args.mpu_only: 
        print('Explicitly disable current and non-current version verification.')
        current = non_current = False

    if non_current:
        response = s3.meta.client.get_bucket_versioning(Bucket=cmd_args.bucket)
        if 'Status' not in response:
            print('Disable non-current version verification for buckets where versioning has never been enabled.')
            non_current = False

    if current or non_current:
        if current: 
          print('Starting LC Processor verification for current objects.')
        if non_current: 
          print('Starting LC Processor verification for noncurrent objects.')
        lc_obj_processor(ctx, current, non_current, rules)
    else:
        print('LC Processor for current and noncurrent objects not enabled.')

    if mpu:
        print('Starting LC Processor verification for incomplete multipart uploads.')
        lc_mpu_processor(ctx, rules)
    else: 
        print('LC Processor for multi part uploads not enabled.')

    print('%d DONE %s %s %s' % (time.time(), ctx.current_counter, ctx.noncurrent_counter, ctx.mpu_counter))

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bucket', action="store", required=True, type=str, help='The bucket name')
    parser.add_argument('--config', action='store', type=str, default=None, help='Configuration file with additional arguments.')
    parser.add_argument('--token', action="store", type=str, default="", help='Next continuation token.')
    parser.add_argument('--current_cnt', action=TupleAction, help="Current expired/total counters, 4 integers separated by comma. Defaults to '0,0,0,0'")
    parser.add_argument('--noncurrent_cnt', action=TupleAction, help="Noncurrent expired/total counters, 4 integers separated by comma. Defaults to '0,0,0,0'")
    parser.add_argument('--heartbeat', action="store", type=int, default=100000, help='Reporting interval. Defaults to 100.000')
    parser.add_argument('--mpu_only', action='store_true', help='Run verification for incomplete multipart uploads only.')

    cmd_args = parser.parse_args() 

    main()
    sys.exit(0)

class TestContext(LCContext): 
    def __init__(self, current_counter: LifecycleCounter, noncurrent_counter: LifecycleCounter, mpu_counter: LifecycleCounter):
        super().__init__(current_counter, noncurrent_counter, mpu_counter)  
        self.rules = []
        self.testmode = True

class TestSetup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        response = {
            'Rules': [
                {
                    'ID': 'delete-objects-with-complex-and-filter',
                    'Status': 'Enabled',
                    'Filter': {
                        'And': {
                            'Prefix': 'logs/',
                            'Tags': [{'Key': 'Environment', 'Value': 'Production'}],
                            'ObjectSizeGreaterThan': 1000,
                            'ObjectSizeLessThan': 100000
                        }
                    },
                    'Expiration': {'Date': datetime(2024, 2, 29, 0, 0, tzinfo=tzutc())}
                }, # rule 0
                {
                    'ID': 'delete-mp-uploads',
                    'Status': 'Enabled',
                    'Filter': {
                        'Prefix': 'logs/',
                    },
                    'AbortIncompleteMultipartUpload': {'DaysAfterInitiation': 90}
                }, # rule 1
                {
                    'ID': 'delete-prior-versions',
                    'Status': 'Enabled',
                    'NoncurrentVersionExpiration': {'NoncurrentDays': 10, 'NewerNoncurrentVersions': 4}
                } # rule 2
            ]
        }
        ctx = TestContext(
            LifecycleCounter('TestCurrent', (0,0,0,0)),
            LifecycleCounter('TestNoncurrent', (0,0,0,0)),
            LifecycleCounter('TestMPU', (0,0,0,0)))
        for rule_data in response['Rules']:
            rule = LifecycleRule(ctx, rule_data)
            ctx.rules.append(rule)
            rule.display()
        cls.ctx = ctx
        print('----------------------------------------------------------------------')

class TestLCPolicy(TestSetup):
    def test_rule_parser(self):
        self.assertEqual(len(self.ctx.rules), 3)

        self.assertEqual(str(self.ctx.rules[0]), 'delete-objects-with-complex-and-filter:Enabled,Prefix:logs/,Size:[1000,100000],Expiration:[2024-03-01 00:00:00+00:00,None,None],Noncurrent_Versions:None')

        cutoff_date = datetime.utcnow() - timedelta(days=90)
        mpu_date = Utils.round_to_next_midnight_utc(cutoff_date)
        self.assertEqual(str(self.ctx.rules[1]), f'delete-mp-uploads:Enabled,Prefix:logs/,Size:[None,None],Expiration:[None,None,{mpu_date}],Noncurrent_Versions:None')

        cutoff_date = datetime.utcnow() - timedelta(days=10)
        noncurrent_date = Utils.round_to_next_midnight_utc(cutoff_date)
        self.assertEqual(str(self.ctx.rules[2]), f'delete-prior-versions:Enabled,Prefix:None,Size:[None,None],Expiration:[None,{noncurrent_date},None],Noncurrent_Versions:4')
        print('test_rule_parser: Passed')

    def test_current(self):
        """
        expire current object with prefix starting as 'logs/' i
        -and- object size less than 1000 
        -and- object size bigger than 100000 
        -and- modification time older when midnight March 1 2024
        {
            'ID': 'delete-objects-with-complex-and-filter',
            'Status': 'Enabled',
            'Filter': {
                'And': {
                    'Prefix': 'logs/',
                    'Tags': [{'Key': 'Environment', 'Value': 'Production'}],
                    'ObjectSizeGreaterThan': 1000,
                    'ObjectSizeLessThan': 100000
                }
            },
            'Expiration': {'Date': datetime(2024, 2, 29, 0, 0, tzinfo=tzutc())}
        }, # rule 0

        """
        # size > 1000 < 100000
        self.ctx.current_counter.count_total(1000)
        self.ctx.current_counter.count_total(1001)
        self.ctx.current_counter.count_total(99999)
        self.ctx.current_counter.count_total(100000)
        self.assertFalse(self.ctx.rules[0].expire_current({'Key': 'logs/5', 'LastModified': datetime(2024, 2, 29, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 1000}))
        self.assertTrue(self.ctx.rules[0].expire_current({'Key': 'logs/5', 'LastModified': datetime(2024, 2, 29, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 1001}))
        self.assertTrue(self.ctx.rules[0].expire_current({'Key': 'logs/5', 'LastModified': datetime(2024, 2, 29, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 99999}))
        self.assertFalse(self.ctx.rules[0].expire_current({'Key': 'logs/5', 'LastModified': datetime(2024, 2, 29, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 100000}))

        # time > Feb-29
        self.ctx.current_counter.count_total(1000)
        self.ctx.current_counter.count_total(1001)
        self.ctx.current_counter.count_total(99999)
        self.ctx.current_counter.count_total(100000)
        self.assertFalse(self.ctx.rules[0].expire_current({'Key': 'logs/5', 'LastModified': datetime(2024, 3, 1, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 1000}))
        self.assertFalse(self.ctx.rules[0].expire_current({'Key': 'logs/5', 'LastModified': datetime(2024, 3, 1, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 1001}))
        self.assertFalse(self.ctx.rules[0].expire_current({'Key': 'logs/5', 'LastModified': datetime(2024, 3, 1, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 99999}))
        self.assertFalse(self.ctx.rules[0].expire_current({'Key': 'logs/5', 'LastModified': datetime(2024, 3, 1, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 100000}))

        # prefix match 
        self.ctx.current_counter.count_total(1001)
        self.ctx.current_counter.count_total(99999)
        self.assertFalse(self.ctx.rules[0].expire_current({'Key': 'Logs/5', 'LastModified': datetime(2024, 2, 29, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 1001}))
        self.assertFalse(self.ctx.rules[0].expire_current({'Key': 'xxxx/5', 'LastModified': datetime(2024, 2, 29, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 99999}))


        # noop for rule 1 and 2
        self.assertFalse(self.ctx.rules[1].expire_current({'Key': 'logs/5', 'LastModified': datetime(2024, 2, 29, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 99999}))
        self.assertFalse(self.ctx.rules[2].expire_current({'Key': 'logs/5', 'LastModified': datetime(2024, 2, 29, 3, 17, 59, 159000, tzinfo=tzutc()), 'Size': 99999}))

        self.assertEqual('TestCurrent:{Expired:[2,101000]},{Total:[10,505000]},{Rate:[20.0%,20.0%]}', str(self.ctx.current_counter))
        print('test_current: Passed')

    def test_noncurrent(self):
        """
        do not expire current version 
        expire versions beyond revision 4 regardless of non current days 
        expire versions older than 10 days midnight cutoff 

        {
            'ID': 'delete-prior-versions',
            'Status': 'Enabled',
            'NoncurrentVersionExpiration': {'NoncurrentDays': 10, 'NewerNoncurrentVersions': 4}
        } # rule 2
        """        
        day1_ago = datetime.utcnow() - timedelta(days=1)
        # do not count current rev size 
        self.ctx.noncurrent_counter.count_total(1) # rev 2
        self.ctx.noncurrent_counter.count_total(1) # rev 3
        self.ctx.noncurrent_counter.count_total(1) # rev 4 
        self.ctx.noncurrent_counter.count_total(10) # rev 5
        self.ctx.noncurrent_counter.count_total(10) # rev 6 
        versions = [
          { 'Size': 100000, 'Key': 'vgoot/1', 'IsLatest': True, 'LastModified': day1_ago },  # rev 1  size should not be accounted in total size 
          { 'Size': 1, 'Key': 'vgoot/1', 'IsLatest': False, 'LastModified': day1_ago },      # rev 2  ok 
          { 'Size': 1, 'Key': 'vgoot/1', 'IsLatest': False, 'LastModified': day1_ago },      # rev 3  ok 
          { 'Size': 1, 'Key': 'vgoot/1', 'IsLatest': False, 'LastModified': day1_ago },      # rev 4  ok 
          { 'Size': 10, 'Key': 'vgoot/1', 'IsLatest': False, 'LastModified': day1_ago },     # rev 5  expired since Noncurrent Newer Version > 4
          { 'Size': 10, 'Key': 'vgoot/1', 'IsLatest': False, 'LastModified': day1_ago },     # rev 6  expired since Noncurrent Newer Version > 4
        ]
        self.assertTrue(self.ctx.rules[2].expire_noncurrent(versions))
        self.assertEqual('TestNoncurrent:{Expired:[2,20]},{Total:[5,23]},{Rate:[40.0%,87.0%]}', str(self.ctx.noncurrent_counter))

        day11_ago = datetime.utcnow() - timedelta(days=11)
        self.ctx.noncurrent_counter.count_total(100) # rev 2
        self.ctx.noncurrent_counter.count_total(1000) # rev 3
        self.ctx.noncurrent_counter.count_total(1000) # rev 4 
        versions = [
          { 'Size': 100000, 'Key': 'vgoot/1', 'IsLatest': True, 'LastModified': day1_ago },  # rev 1  size should not be accounted in total size 
          { 'Size': 100, 'Key': 'vgoot/1', 'IsLatest': False, 'LastModified': day1_ago },      # rev 2  ok 
          { 'Size': 1000, 'Key': 'vgoot/1', 'IsLatest': False, 'LastModified': day11_ago },      # rev 3  expired since NoncurrentDays > 10
          { 'Size': 1000, 'Key': 'vgoot/1', 'IsLatest': False, 'LastModified': day11_ago },      # rev 4  expired since NoncurrentDays > 10
        ]    

        self.assertTrue(self.ctx.rules[2].expire_noncurrent(versions))
        # noop for rule 0 and 1
        self.assertFalse(self.ctx.rules[0].expire_noncurrent(versions))
        self.assertFalse(self.ctx.rules[1].expire_noncurrent(versions))
        self.assertEqual('TestNoncurrent:{Expired:[4,2020]},{Total:[8,2123]},{Rate:[50.0%,95.1%]}', str(self.ctx.noncurrent_counter))

        # do not expire current version;  
        versions = [
          { 'Size': 100000, 'Key': 'vgoot/1', 'IsLatest': True, 'LastModified': day11_ago },  # rev 1  size should not be accounted in total size 
        ]
        self.assertFalse(self.ctx.rules[2].expire_noncurrent(versions))
        self.assertEqual('TestNoncurrent:{Expired:[4,2020]},{Total:[8,2123]},{Rate:[50.0%,95.1%]}', str(self.ctx.noncurrent_counter))
        print('test_noncurrent: Passed')

    def test_mpu(self):
        """
        expire incomplete Multi Part Uploads older when 90 days matching prefix 'logs/'
        {
            'ID': 'delete-mp-uploads',
            'Status': 'Enabled',
            'Filter': {
                'Prefix': 'logs/',
            },
            'AbortIncompleteMultipartUpload': {'DaysAfterInitiation': 90}
        }, # rule 1
        """ 

        day89_ago = datetime.utcnow() - timedelta(days=89)
        self.ctx.mpu_counter.count_total(100)
        self.assertFalse(self.ctx.rules[1].expire_mpu({'UploadId': '2~jG3hATFhjzgSzGd2No7KZQmXXvVFFMg', 'Key': 'logs/', 'Initiated': day89_ago}, 100))
        day90_ago = datetime.utcnow() - timedelta(days=90)
        self.ctx.mpu_counter.count_total(1000)
        self.assertTrue(self.ctx.rules[1].expire_mpu({'UploadId': '2~jG3hATFhjzgSzGd2No7KZQmXXvVFFMg', 'Key': 'logs/', 'Initiated': day90_ago}, 1000))
        self.ctx.mpu_counter.count_total(100)
        self.assertFalse(self.ctx.rules[1].expire_mpu({'UploadId': '2~jG3hATFhjzgSzGd2No7KZQmXXvVFFMg', 'Key': 'Logs/', 'Initiated': day90_ago}, 100))
        self.ctx.mpu_counter.count_total(100)
        self.assertFalse(self.ctx.rules[1].expire_mpu({'UploadId': '2~jG3hATFhjzgSzGd2No7KZQmXXvVFFMg', 'Key': 'xxxx', 'Initiated': day90_ago}, 100))

        self.assertEqual('TestMPU:{Expired:[1,1000]},{Total:[4,1300]},{Rate:[25.0%,76.9%]}', str(self.ctx.mpu_counter))
        print('test_mpu: Passed')

