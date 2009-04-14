import sys
import os
import tempfile
  

from tools import Tools

class Demuxer:
  def __init__(self,tools,filename,type):
    if not self.CreateDirectory():
      raise Exception, 'Failed to create directory'
    self.Demux(tools,filename,type)

  def GetDirectory(self):
    return self.directory

  def CreateDirectory(self):
    try:
      self.directory=tempfile.mkdtemp(dir='.',prefix='katedj-tmp-extract-')
    except OSError,e:
      print 'Failed to create temporary directory'
      return False
    return True

  def GetCodecs(self,tools):
    cmdline=tools.codecs_command
    if cmdline!=None:
      f=os.popen(cmdline)
      if f:
        list=[]
        line=f.readline()
        while line:
          line=line.split('\n')[0]
          if line and 'Kate' not in line:
            list.append(line)
          line=f.readline()
        f.close()
        return list
    return ['theora','vorbis','dirac','speex','flac','cmml']
    
  def DemuxMisc(self,tools,filename):
    params=[]
    params+=['-o',os.path.join(self.directory,'misc.ogg')]
    for codec in self.GetCodecs(tools):
      params+=['-c',codec]
    params+=[filename]
    tools.run_demux(params)

  def DemuxKate(self,tools,filename,type):
    params=[]
    params+=['-o',os.path.join(self.directory,'kate.%l.%c.%i.%s.kate')]
    params+=['-t',type]
    params+=[filename]
    tools.run_katedec(params)

  def Demux(self,tools,filename,type):
    self.DemuxMisc(tools,filename)
    self.DemuxKate(tools,filename,type)

if __name__=='__main__':
  tools=Tools()
  file='../../built-streams/demo.ogg'
  if len(sys.argv)>1:
    file=sys.argv[1];
  demuxer=Demuxer(tools,file)
