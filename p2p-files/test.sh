#echo "filename" > $1/"out" 
#ls -l $1 | tail -n +2 | while read -r -a arr
#	do
#	#echo  $((arr[0]))
#	echo -e "${arr[8]}\t${arr[7]}\t${arr[0]}" >> $1/out
#	done

cp pretty/out .
tail -n +2 | sort -r -k $1 out	
