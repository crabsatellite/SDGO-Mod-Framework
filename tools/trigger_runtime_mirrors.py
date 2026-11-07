"""Request configured pull-mirror automation after GitHub runtime publication."""
import json,os,subprocess


def main():
    path=os.environ['RUNTIME_MIRROR_DISPATCH_PATH'];repositories=json.loads(os.environ['RUNTIME_MIRROR_REPOSITORIES'])
    parts=path.split('/')
    if len(parts)!=4 or parts[0]!='repos'or parts[-1]!='dispatches':raise ValueError('Invalid configured mirror automation path')
    if not isinstance(repositories,list)or not 1<=len(repositories)<=8 or any(not str(x).isdigit()for x in repositories):raise ValueError('Invalid configured mirror identities')
    env={**os.environ,'GH_TOKEN':os.environ['RUNTIME_GITHUB_MIRROR_TOKEN']}
    for index,identity in enumerate(repositories):
        body={'event_type':'sync-codeup','client_payload':{'repository_id':str(identity)}}
        result=subprocess.run(['gh','api','--method','POST',path,'--input','-'],input=json.dumps(body).encode(),env=env,capture_output=True,timeout=60)
        if result.returncode:raise RuntimeError('Configured pull-mirror request failed for target '+str(index))
    print('Requested configured pull mirrors; delivered refs must still be read back',flush=True)


if __name__=='__main__':main()
